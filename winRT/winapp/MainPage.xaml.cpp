﻿//
// MainPage.xaml.cpp
// Implementation of the MainPage class.
//

#include "pch.h"
#include "MainPage.xaml.h"

#define LOOK_AHEAD 10 /* A +/- count on the pages to pre-render */
#define MIN_SCALE 0.5
#define MAX_SCALE 4
#define MARGIN_BUFF 400

typedef struct RectSize_s
{
    float width;
    float height;
} RectSize;

static float screenScale = 1;
static fz_context *ctx = NULL;

using namespace winapp;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;

//****************** Added *****************
using namespace Windows::Storage::Pickers;
using namespace Windows::Devices::Enumeration;
using namespace concurrency;
using namespace Windows::Graphics::Imaging;
//****************** End Add ****************

typedef struct win_stream_struct_s
{
    IRandomAccessStream^ stream;
} win_stream_struct;
static win_stream_struct win_stream;

MainPage::MainPage()
{
	InitializeComponent();
    m_currpage = 0;
    m_file_open = false;
    m_doc = NULL;
    m_slider_min = 0;
    m_slider_max = 0;
    m_init_done = false;
    m_memory_use = 0;
    m_zoom_mode = false;
    m_zoom_handled = false;
    m_first_time = false;
    m_insearch = false;

    m_curr_zoom = 1.0;
    m_canvas_translate.X = 0;
    m_canvas_translate.Y = 0;

    this->xaml_PageSlider->Minimum = m_slider_min;
    this->xaml_PageSlider->Maximum = m_slider_max;
    this->xaml_PageSlider->IsEnabled = false;

	// use at most 128M for resource cache
	ctx = fz_new_context(NULL, NULL, 128<<20);

    // Create the flipview object
    m_flipView = ref new FlipView();
    m_flipView->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    m_flipView->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
    m_flipView->SelectionChanged += 
                    ref new SelectionChangedEventHandler(this, &MainPage::FlipView_SelectionChanged);
     m_flipView->DoubleTapped +=
        ref new DoubleTappedEventHandler(this, &MainPage::FlipView_Double);
     
    // Create the image brush
    m_renderedImage = ref new ImageBrush();
}

/// <summary>
/// Invoked when this page is about to be displayed in a Frame.
/// </summary>
/// <param name="e">Event data that describes how this page was reached.  The Parameter
/// property is typically used to configure the page.</param>
void MainPage::OnNavigatedTo(NavigationEventArgs^ e)
{
	(void) e;	// Unused parameter
}

void winapp::MainPage::Picker(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	FileOpenPicker^ openPicker = ref new FileOpenPicker();
	openPicker->ViewMode = PickerViewMode::List;
	openPicker->SuggestedStartLocation = PickerLocationId::PicturesLibrary;
    openPicker->FileTypeFilter->Append(".pdf");
    openPicker->FileTypeFilter->Append(".xps");
    openPicker->FileTypeFilter->Append(".oxps");

	create_task(openPicker->PickSingleFileAsync()).then([this](StorageFile^ file) 
	{ 
		if (file) 
		{ 		
            this->OpenDocument(file);
            /* File selected.  Start rendering and switch view. */
			//txtBlockOutput->Text = "Picked photo: " + file->Name;
		} 
		else 
		{ 
			/* Nothing selected */
		} 
	});
}

void MainPage::NotifyUserFileNotExist()
{
    //NotifyUser("The file '" + Filename + "' does not exist. Use scenario one to create this file.", NotifyType::ErrorMessage);
}

void MainPage::HandleFileNotFoundException(Platform::COMException^ e)
{
    if (e->HResult == 0x80070002) // Catch FileNotExistException
    {
        NotifyUserFileNotExist();
    }
    else
    {
        throw e;
    }
}

static RectSize measurePage(fz_document *doc, fz_page *page)
{
	RectSize pageSize;
    fz_rect rect;
	fz_rect *bounds = fz_bound_page(doc, page, &rect);

	pageSize.width = bounds->x1 - bounds->x0;
	pageSize.height = bounds->y1 - bounds->y0;
	return pageSize;
}

static RectSize fitPageToScreen(RectSize page, RectSize screen)
{
    RectSize pageSize;

	float hscale = screen.width / page.width;
	float vscale = screen.height / page.height;
	float scale = fz_min(hscale, vscale);
    pageSize.width = floorf(page.width * scale) / page.width;
	pageSize.height = floorf(page.height * scale) / page.height;
	return pageSize;
}

void MainPage::Prepare_bmp(int width, int height, DataWriter ^dw)
{
    int row_size = width * 4;
    int bmp_size = row_size * height + 54;

    dw->WriteString("BM");
    dw->ByteOrder = ByteOrder::LittleEndian;
    dw->WriteInt32(bmp_size);
    dw->WriteInt16(0);
    dw->WriteInt16(0);
    dw->WriteInt32(54);
    dw->WriteInt32(40);
    dw->WriteInt32(width);
    dw->WriteInt32(height);
    dw->WriteInt16(1);
    dw->WriteInt16(32);
    dw->WriteInt32(0);
    dw->WriteInt32(row_size * height);
    dw->WriteInt32(2835);
    dw->WriteInt32(2835);
    dw->WriteInt32(0);
    dw->WriteInt32(0);
}

/* Add rendered page into flipview structure at location page_num */
void MainPage::AddPage(int page_num) 
{
    FlipViewItem ^flipview_temp = ref new FlipViewItem();
    flipview_temp->Background = this->m_renderedImage;
    m_flipView->Items->Append(flipview_temp);
}

/* Replace rendered page into flipview structure at location page_num */
void MainPage::ReplacePage(int page_num) 
{
    FlipViewItem ^flipview_temp = (FlipViewItem^) m_flipView->Items->GetAt(page_num);
    flipview_temp->Background = this->m_renderedImage;
}

/* Add rendered page into flipview structure at location page_num */
void MainPage::AddBlankPage(int page_num) 
{
    FlipViewItem ^flipview_temp = ref new FlipViewItem();
    flipview_temp->Background = this->m_blankPage;
    m_flipView->Items->Append(flipview_temp);
}

/* Create white image for us to use as place holder in large document for flip
   view filling instead of the thumbnail image  */
void MainPage::CreateBlank(int width, int height)
{
    Platform::Array<unsigned char>^ bmp_data = 
            ref new Platform::Array<unsigned char>(height * 4 * width);
    /* Set up the memory stream */
    WriteableBitmap ^bmp = ref new WriteableBitmap(width, height);
    InMemoryRandomAccessStream ^ras = ref new InMemoryRandomAccessStream();
    DataWriter ^dw = ref new DataWriter(ras->GetOutputStreamAt(0));
    /* Go ahead and write our header data into the memory stream */
    this->Prepare_bmp(width, height, dw);

    /* Set the data to all white */
    memset(bmp_data->Data, 255, height * 4 * width);

    /* Write the data */
    dw->WriteBytes(bmp_data);

    DataWriterStoreOperation^ result = dw->StoreAsync();
    /* Block on the Async call */
    while(result->Status != AsyncStatus::Completed) {
    }
    /* And store in a the image brush */
    bmp->SetSource(ras);
    m_blankPage = ref new ImageBrush();
    m_blankPage->Stretch = Windows::UI::Xaml::Media::Stretch::None;
    m_blankPage->ImageSource = bmp;
}

/* win_read_file.  Reading of windows managed stream.  This is not ideal as I have 
   to read into a managed buffer and then transfer to the actual buffer I want.  I
   would like a more direct approach */
static int win_read_file(fz_stream *stm, unsigned char *buf, int len)
{
    void *temp = stm->state;
    win_stream_struct *stream = reinterpret_cast <win_stream_struct*> (temp);
    IRandomAccessStream^ Stream = stream->stream;
    unsigned long long curr_pos = Stream->Position;
    unsigned long long length = Stream->Size;
    
    DataReader^ local_reader = ref new DataReader(Stream);
    DataReaderLoadOperation^ result = local_reader->LoadAsync(len);

    /* Block on the Async call */
    while(result->Status != AsyncStatus::Completed) {

    }
    result->GetResults();
    int curr_len2 = local_reader->UnconsumedBufferLength;
    if (curr_len2 < len)
        len = curr_len2;

    Platform::Array<unsigned char>^ arrByte = ref new Platform::Array<unsigned char>(len);
    local_reader->ReadBytes(arrByte);

    memcpy(buf, arrByte->Data, len);
    local_reader->DetachStream();

	return len;
}

static void win_seek_file(fz_stream *stm, int offset, int whence)
{
    void *temp = stm->state;
    win_stream_struct *stream = reinterpret_cast <win_stream_struct*> (temp);
    IRandomAccessStream^ Stream = stream->stream;
    unsigned long long curr_pos = Stream->Position;
    unsigned long long length = Stream->Size;
    unsigned long long n;

    if (whence == SEEK_END) 
    {
        n = length + offset;
    } 
    else if (whence == SEEK_CUR)
    {
        n = curr_pos + offset;
    }
    else if (whence == SEEK_SET)
    {
        n = offset;
    } 
    Stream->Seek(n);
    curr_pos = Stream->Position;
    stm->pos = n;
	stm->rp = stm->bp;
	stm->wp = stm->bp;
}

static void win_close_file(fz_context *ctx, void *state)
{

    DataReader^ dataReader = reinterpret_cast <DataReader^> (state);

    delete dataReader;
}

void winapp::MainPage::PixToMemStream(fz_pixmap *pix, DataWriter ^dw, Platform::Array<unsigned char> ^arr)
{
	unsigned char *samples = fz_pixmap_samples(ctx, pix);
	int w = fz_pixmap_width(ctx, pix);
	int h = fz_pixmap_height(ctx, pix);

    /* Write the data */
    dw->WriteBytes(arr);

    DataWriterStoreOperation^ result = dw->StoreAsync();
    /* Block on the Async call */
    while(result->Status != AsyncStatus::Completed) {
    }
}

void winapp::MainPage::RenderPage(fz_document *doc, fz_page *page, int *width, int *height, double scale_factor)
{
	fz_matrix ctm, *pctm = &ctm;
	fz_device *dev;
	fz_pixmap *pix;
    RectSize pageSize;
    RectSize scale;
    RectSize screenSize;
    int bmp_width, bmp_height; 

    screenSize.height = this->ActualHeight;
    screenSize.width = this->ActualWidth;

	screenSize.width *= screenScale;
	screenSize.height *= screenScale;
    
    pageSize = measurePage(doc, page);
	scale = fitPageToScreen(pageSize, screenSize);
	pctm = fz_scale(pctm, scale.width * scale_factor, scale.height * scale_factor);
    bmp_width = pageSize.width * scale.width * scale_factor;
    bmp_height = pageSize.height * scale.height * scale_factor;

    /* Y is flipped for some reason */
    ctm.f = bmp_height;
    ctm.d = -ctm.d;

    /* Allocate space for bmp */
    Platform::Array<unsigned char>^ bmp_data = 
            ref new Platform::Array<unsigned char>(bmp_height * 4 * bmp_width);
    m_memory_use += bmp_height * 4 * bmp_width;
    /* Set up the memory stream */
    WriteableBitmap ^bmp = ref new WriteableBitmap(bmp_width, bmp_height);
    InMemoryRandomAccessStream ^ras = ref new InMemoryRandomAccessStream();
    DataWriter ^dw = ref new DataWriter(ras->GetOutputStreamAt(0));
    /* Go ahead and write our header data into the memory stream */
    this->Prepare_bmp(bmp_width, bmp_height, dw);
    /* Now get a pointer to our samples and pass it to fitz to use */
    pix = fz_new_pixmap_with_data(ctx, fz_device_bgr, bmp_width, bmp_height, &(bmp_data[0]));
	fz_clear_pixmap_with_value(ctx, pix, 255);
	dev = fz_new_draw_device(ctx, pix);
	fz_run_page(doc, page, dev, pctm, NULL);
	fz_free_device(dev);
    /* Now the data into the memory stream */
	PixToMemStream(pix, dw, bmp_data);
    /* And store in a new image brush */
    bmp->SetSource(ras);
    m_renderedImage = ref new ImageBrush();
    m_renderedImage->Stretch = Windows::UI::Xaml::Media::Stretch::None;
    m_renderedImage->ImageSource = bmp;
    *width = bmp_width;
    *height = bmp_height;
}

void winapp::MainPage::SetupCanvas()
{
    int height = this->ActualHeight;
    int width = this->ActualWidth;

    m_Canvas = ref new Canvas();

    m_Canvas->Height =  height;        
    m_Canvas->Width = width;

    m_Canvas->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    m_Canvas->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
    m_Canvas->ManipulationMode = Windows::UI::Xaml::Input::ManipulationModes::All;
    m_Canvas->ManipulationDelta += 
       ref new ManipulationDeltaEventHandler(this, &MainPage::Canvas_ManipulationDelta);
    m_Canvas->ManipulationStarted +=
      ref new ManipulationStartedEventHandler(this, &MainPage::Canvas_ManipulationStarted);
    m_Canvas->ManipulationStarting +=
       ref new ManipulationStartingEventHandler(this, &MainPage::Canvas_ManipulationStarting);
    m_Canvas->DoubleTapped +=
        ref new DoubleTappedEventHandler(this, &MainPage::Canvas_Double);

    CreateBlank(width, height);
    m_Canvas->Background = this->m_blankPage;
    m_Canvas->Children->Append(m_flipView);
    xaml_MainGrid->Children->Append(m_Canvas);
    m_Canvas->Background->Opacity = 0;
}

void winapp::MainPage::OpenDocument(StorageFile^ file)
{
    String^ path = file->Path;
    const wchar_t *w = path->Data();
    int size = wcslen(w);

    /* Set up the canvas */
    this->SetupCanvas();

    create_task(file->OpenAsync(FileAccessMode::Read)).then([this, file](task<IRandomAccessStream^> task)
    {
        try
        {
            IRandomAccessStream^ readStream = task.get();
            UINT64 const size = readStream->Size;
            win_stream.stream = readStream;
            
            if (size <= MAXUINT32)
            {
                /* assign data reader to stream object */
                fz_stream *str;

                str =  fz_new_stream(ctx, 0, win_read_file, win_close_file);
                str->seek = win_seek_file;
                str->state =  reinterpret_cast <void*> (&win_stream);
                    
                /* Now lets see if we can render the file */
                m_doc = fz_open_document_with_stream(ctx, "pdf", str);
                m_num_pages = m_doc->count_pages(m_doc);

                if ((m_currpage) >= m_num_pages) 
                {
                    m_currpage = m_num_pages - 1;
                } 
                else if (m_currpage < 0) 
                {
                    m_currpage = 0;
                }
                /* Do a few pages */
                int height, width;
                for (int k = 0; k < LOOK_AHEAD + 2; k++) 
                {
                    if (m_num_pages > k ) 
                    {
                        fz_page *page = fz_load_page(m_doc, k);
			            this->RenderPage(m_doc, page, &width, &height, 1);
                        AddPage(k);
                        fz_free_page(m_doc, page);
                    }
                }
                /* If we still have more pages, then set the rest to a blank white
                   page which will get bumped as we move through the doc. */
                if (m_num_pages > LOOK_AHEAD + 2)
                {
                    CreateBlank(width, height);
                    for (int k = LOOK_AHEAD + 2; k < m_num_pages; k++) 
                    {
                        AddBlankPage(k);
                    }
                }
                /* Update the slider settings, if more than one page */
                if (m_num_pages > 1) 
                {
                    this->xaml_PageSlider->Maximum = m_num_pages;
                    this->xaml_PageSlider->Minimum = 1;
                    this->xaml_PageSlider->IsEnabled = true;
                } 
                else
                {
                    this->xaml_PageSlider->Maximum = 0;
                    this->xaml_PageSlider->Minimum = 0;
                    this->xaml_PageSlider->IsEnabled = false;
                }
                this->m_init_done = true;
            }
            else
            {
                delete readStream; 
            }
        }
        catch(COMException^ ex) {
            this->HandleFileNotFoundException(ex);
        }
    });
}

void winapp::MainPage::RenderRange(int curr_page)
{
    /* Render +/- the look ahead from where we are if blank page is present */
    for (int k = curr_page - LOOK_AHEAD; k <= curr_page + LOOK_AHEAD; k++) 
    {
        if (k >= 0 && k < m_num_pages) 
        {
            FlipViewItem ^flipview_temp = (FlipViewItem^) m_flipView->Items->GetAt(k);
            if (flipview_temp->Background == this->m_blankPage) 
            {
                fz_page *page = fz_load_page(m_doc, k);
                int width, height;
			    this->RenderPage(m_doc, page, &width, &height, 1);
                ReplacePage(k);
                fz_free_page(m_doc, page);
            } 
        }
    }
}

void winapp::MainPage::Slider_ValueChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e)
{
    int newValue= (int) this->xaml_PageSlider->Value - 1;  /* zero based */

    if (m_init_done && this->xaml_PageSlider->IsEnabled) 
    {
        this->m_flipView->SelectedIndex = newValue;
        this->RenderRange(newValue);
    }
     Windows::UI::Xaml::Input::ManipulationModes temp = m_flipView->ManipulationMode;
}

void winapp::MainPage::FlipView_SelectionChanged(Object^ sender, SelectionChangedEventArgs^ e)
{
    int pos = this->m_flipView->SelectedIndex;

    if (m_init_done)
        this->RenderRange(pos);
}

void winapp::MainPage::FlipView_Double(Object^ sender, DoubleTappedRoutedEventArgs^ e)
{
    if (!m_zoom_mode)
    {
        m_zoom_mode = true;
        int pos = this->m_flipView->SelectedIndex;
        FlipViewItem ^flipview_temp = (FlipViewItem^) m_flipView->Items->GetAt(pos);
        ImageBrush^ Image = (ImageBrush^) (flipview_temp->Background);
        m_flipView->IsEnabled = false;
        this->m_Canvas->Background = Image;
        this->m_Canvas->Background->Opacity = 1;
        this->m_flipView->Opacity = 0;
        m_zoom_handled = true;
        m_first_time = true;
    }
}

void winapp::MainPage::Canvas_ManipulationStarting(Object^ sender, ManipulationStartingRoutedEventArgs^ e)
{
    bool handled;

    e->GetType();
    handled = e->Handled;
}

void winapp::MainPage::Canvas_ManipulationStarted(Object^ sender, ManipulationStartedRoutedEventArgs^ e)
{
    this->m_touchpoint = e->Position;
}

void winapp::MainPage::Canvas_ManipulationDelta(Object^ sender, ManipulationDeltaRoutedEventArgs^ e)
{
    int width, height;

    m_changes = e->Cumulative;
    if (e->Delta.Scale != 1 || m_first_time) 
    {
        /* Render at scaled resolution */
        int pos = this->m_flipView->SelectedIndex;
        fz_page *page = fz_load_page(m_doc, pos);
        m_curr_zoom = m_curr_zoom * e->Delta.Scale;
        if (m_curr_zoom < MIN_SCALE) m_curr_zoom = MIN_SCALE;
        if (m_curr_zoom > MAX_SCALE) m_curr_zoom = MAX_SCALE;
        this->RenderPage(m_doc, page, &width, &height, m_curr_zoom);
        this->m_Canvas->Background = this->m_renderedImage;
        m_zoom_size.X = width;
        m_zoom_size.Y = height;
        m_first_time = false;
    }

    Windows::UI::Xaml::Media::TranslateTransform ^trans_transform = ref new TranslateTransform();
    m_canvas_translate.X += e->Delta.Translation.X;
    m_canvas_translate.Y += e->Delta.Translation.Y;
    
    if (m_canvas_translate.Y > ((this->ActualHeight + m_zoom_size.Y) / 2 - MARGIN_BUFF) ) 
    {
        m_canvas_translate.Y = (this->ActualHeight + m_zoom_size.Y) / 2 - MARGIN_BUFF;
    }
    if (m_canvas_translate.Y < (MARGIN_BUFF - (this->ActualHeight + m_zoom_size.Y) / 2) ) 
    {
        m_canvas_translate.Y = MARGIN_BUFF - (this->ActualHeight + m_zoom_size.Y) / 2;
    }
    if (m_canvas_translate.X > ((this->ActualWidth + m_zoom_size.X) / 2 - MARGIN_BUFF)) 
    {
        m_canvas_translate.X = (this->ActualWidth + m_zoom_size.X) / 2 - MARGIN_BUFF;
    }

    if (m_canvas_translate.X < (MARGIN_BUFF - (this->ActualWidth + m_zoom_size.X) / 2)) 
    {
        m_canvas_translate.X = (MARGIN_BUFF - (this->ActualWidth + m_zoom_size.X) / 2);
    } 

    trans_transform->X = m_canvas_translate.X;
    trans_transform->Y = m_canvas_translate.Y;
    this->m_Canvas->Background->Transform = trans_transform;
}

void winapp::MainPage::Canvas_Double(Object^ sender, DoubleTappedRoutedEventArgs^ e)
{
    if (m_zoom_mode && !m_zoom_handled)
    {
        m_zoom_mode = false;
        FlipViewItem ^flipview_temp = (FlipViewItem^) m_flipView->Items->GetAt(this->m_currpage);
        this->m_Canvas->Background->Opacity = 0;
        this->m_flipView->Opacity = 1;
        m_flipView->IsEnabled = true;
        m_first_time = true;
    }
    m_zoom_handled = false;
    m_curr_zoom = 1.0;
    m_canvas_translate.X = 0;
    m_canvas_translate.Y = 0;
}

/* Search Related Code */

static int hit_count = 0;
static fz_rect hit_bbox[500];

static int
search_page(fz_document *doc, int number, char *needle, fz_cookie *cookie)
{
	fz_page *page = fz_load_page(doc, number);

	fz_text_sheet *sheet = fz_new_text_sheet(ctx);
	fz_text_page *text = fz_new_text_page(ctx, &fz_empty_rect);
	fz_device *dev = fz_new_text_device(ctx, sheet, text);
	fz_run_page(doc, page, dev, &fz_identity, cookie);
	fz_free_device(dev);

	hit_count = fz_search_text_page(ctx, text, needle, hit_bbox, nelem(hit_bbox));;

	fz_free_text_page(ctx, text);
	fz_free_text_sheet(ctx, sheet);
	fz_free_page(doc, page);

	return hit_count;
}

void winapp::MainPage::Searcher(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    /* Update the app bar so that we can do the search */
    StackPanel^ leftPanel = (StackPanel^) this->TopAppBar->FindName("LeftPanel");

	if (leftPanel != nullptr && m_insearch)
    {
        m_insearch = false;
        leftPanel->Children->RemoveAtEnd();
        leftPanel->Children->RemoveAtEnd();
        leftPanel->Children->RemoveAtEnd();
    }
    else if (leftPanel != nullptr && !m_insearch)
	{
        m_insearch = true;
	    Windows::UI::Xaml::Controls::Button^ PrevButton = ref new Button();
        PrevButton->Style = safe_cast<Windows::UI::Xaml::Style^>(App::Current->Resources->Lookup("PreviousAppBarButtonStyle"));
	    PrevButton->Click += ref new RoutedEventHandler(this, &winapp::MainPage::SearchPrev);
        
	    Windows::UI::Xaml::Controls::Button^ NextButton = ref new Button();
        NextButton->Style = safe_cast<Windows::UI::Xaml::Style^>(App::Current->Resources->Lookup("NextAppBarButtonStyle"));
	    NextButton->Click += ref new RoutedEventHandler(this, &winapp::MainPage::SearchNext);

        Windows::UI::Xaml::Controls::TextBox^ SearchBox = ref new TextBox();
        SearchBox->Name = "findBox";
        SearchBox->Width = 200;
        SearchBox->Height = 20;
        
        leftPanel->Children->Append(SearchBox);
        leftPanel->Children->Append(PrevButton);
        leftPanel->Children->Append(NextButton);
	}
}

void winapp::MainPage::SearchNext(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    StackPanel^ leftPanel = (StackPanel^) this->TopAppBar->FindName("LeftPanel");
    TextBox^ findBox = (TextBox^) leftPanel->FindName("findBox");
    String^ textToFind = findBox->Text;

}

void winapp::MainPage::SearchPrev(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    StackPanel^ leftPanel = (StackPanel^) this->TopAppBar->FindName("LeftPanel");
    TextBox^ findBox = (TextBox^) leftPanel->FindName("findBox");
    String^ textToFind = findBox->Text;

}