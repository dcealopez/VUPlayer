#include "VUMeter.h"

#include "VUMeterData.h"

// Render thread millisecond interval.
static const DWORD s_RenderThreadInterval = 15;

// Rise factor.
static const float s_RiseFactor = 0.2f;

// Rounded corner width.
static const float s_RoundedCornerWidth = 16.0f;

DWORD WINAPI VUMeter::RenderThreadProc( LPVOID lpParam )
{
	VUMeter* vumeter = reinterpret_cast<VUMeter*>( lpParam );
	if ( nullptr != vumeter ) {
		vumeter->RenderThreadHandler();
	}
	return 0;
}

VUMeter::VUMeter( WndVisual& wndVisual, const bool stereo ) :
	Visual( wndVisual ),
	m_RenderThread( NULL ),
	m_RenderStopEvent( CreateEvent( NULL /*attributes*/, TRUE /*manualReset*/, FALSE /*initialState*/, L"" /*name*/ ) ),
	m_MeterImage( VU_WIDTH * VU_HEIGHT * 4 ),
	m_MeterPin( nullptr ),
	m_BitmapLeft( nullptr ),
	m_BitmapRight( nullptr ),
	m_Brush( nullptr ),
	m_OutputLevel( {} ),
	m_LeftDisplayLevel( 0 ),
	m_RightDisplayLevel( 0 ),
	m_MeterPosition( {} ),
	m_Decay( GetSettings().GetVUMeterDecay() ),
	m_IsStereo( stereo )
{
	memcpy( m_MeterImage.data(), VU_BASE, VU_WIDTH * VU_HEIGHT * 4 );
}

VUMeter::~VUMeter()
{
	StopRenderThread();
	CloseHandle( m_RenderStopEvent );
	CloseHandle( m_RenderThread );
	FreeResources();
}

int VUMeter::GetHeight( const int width )
{
	const int height = static_cast<int>( static_cast<float>( width ) * ( m_IsStereo ? 2 : 1 ) / VU_WIDTH * VU_HEIGHT );
	return height;
}

void VUMeter::Show()
{
	StartRenderThread();
}

void VUMeter::Hide()
{
	StopRenderThread();
}

void VUMeter::StartRenderThread()
{
	if ( nullptr == m_RenderThread ) {
		ResetEvent( m_RenderStopEvent );
		m_RenderThread = CreateThread( NULL /*attributes*/, 0 /*stackSize*/, RenderThreadProc, reinterpret_cast<LPVOID>( this ), 0 /*flags*/, NULL /*threadId*/ );
	}
}

void VUMeter::StopRenderThread()
{
	if ( nullptr != m_RenderThread ) {
		SetEvent( m_RenderStopEvent );
		WaitForSingleObject( m_RenderThread, INFINITE );
		CloseHandle( m_RenderThread );
		m_RenderThread = nullptr;
		m_OutputLevel = {};
		m_LeftDisplayLevel = 0;
		m_RightDisplayLevel = 0;
		m_MeterPosition = {};
	}
}

void VUMeter::RenderThreadHandler()
{
	DWORD result = 0;
	do {
		if ( GetLevels() ) {
			DoRender();
		}
		result = WaitForSingleObject( m_RenderStopEvent, s_RenderThreadInterval );
	} while ( WAIT_OBJECT_0 != result );
}

bool VUMeter::GetLevels()
{
	auto& [ leftOutput, rightOutput ] = m_OutputLevel;
	GetOutput().GetLevels( leftOutput, rightOutput );
	if ( !m_IsStereo ) {
		if ( leftOutput > rightOutput ) {
			rightOutput = leftOutput;
		} else {
			leftOutput = rightOutput;
		}
	}
	leftOutput = std::clamp( leftOutput, 0.0f, 1.0f );
	rightOutput = std::clamp( rightOutput, 0.0f, 1.0f );

	float leftDisplay = m_LeftDisplayLevel;
	float rightDisplay = m_RightDisplayLevel;

	if ( leftDisplay < leftOutput ) {
		leftDisplay += ( leftOutput - leftDisplay ) * s_RiseFactor;
	} else {
		leftDisplay -= m_Decay;
		if ( leftDisplay < leftOutput ) {
			leftDisplay = leftOutput;
		}
	}

	if ( rightDisplay < rightOutput ) {
		rightDisplay += ( rightOutput - rightDisplay ) * s_RiseFactor;
	} else {
		rightDisplay -= m_Decay;
		if ( rightDisplay < rightOutput ) {
			rightDisplay = rightOutput;
		}
	}

	const bool levelsChanged = ( leftDisplay != m_LeftDisplayLevel ) || ( rightDisplay != m_RightDisplayLevel );
	if ( levelsChanged ) {
		m_LeftDisplayLevel = leftDisplay;
		m_RightDisplayLevel = rightDisplay;
	}

	return levelsChanged;
}

void VUMeter::DrawPin( const int position )
{
	int pos = position;
	if ( pos < 0 ) {
		pos = 0;
	} else if ( pos > VU_PINCOUNT ) {
		pos = VU_PINCOUNT;
	}

	const DWORD* pinBuffer = VU_PIN[ pos ];
	if ( m_MeterPin != pinBuffer ) {
		// Blank out the previous pin position.
		memcpy( m_MeterImage.data(), VU_BASE, VU_WIDTH * VU_HEIGHT * 4 );

		// Draw the current pin.
		int index = 0;
		while ( 0 != pinBuffer[ index ] ) {
			m_MeterImage[ pinBuffer[ index ] >> 8 ] = static_cast<BYTE>( pinBuffer[ index ] & 0xff );
			++index;
		}

		m_MeterPin = pinBuffer;
	}
}

void VUMeter::UpdateBitmaps( const float leftLevel, const float rightLevel )
{
	auto& [ leftMeter, rightMeter ] = m_MeterPosition;

	const int leftPosition = GetPinPosition( leftLevel );
	const int rightPosition = GetPinPosition( rightLevel );

	if ( ( leftMeter != leftPosition ) && ( nullptr != m_BitmapLeft ) ) {
		leftMeter = leftPosition;
		DrawPin( leftMeter );
		const D2D1_RECT_U destRect = D2D1::RectU( 0 /*left*/, 0 /*top*/, VU_WIDTH /*right*/, VU_HEIGHT /*bottom*/ );
		const UINT32 pitch = VU_WIDTH * 4;
		m_BitmapLeft->CopyFromMemory( &destRect, m_MeterImage.data(), pitch );
	} 

	if ( ( rightMeter != rightPosition ) && ( nullptr != m_BitmapRight ) ) {
		rightMeter = rightPosition;
		DrawPin( rightMeter );
		const D2D1_RECT_U destRect = D2D1::RectU( 0 /*left*/, 0 /*top*/, VU_WIDTH /*right*/, VU_HEIGHT /*bottom*/ );
		const UINT32 pitch = VU_WIDTH * 4;
		m_BitmapRight->CopyFromMemory( &destRect, m_MeterImage.data(), pitch );
	}
}

void VUMeter::OnPaint()
{
	ID2D1DeviceContext* deviceContext = BeginDrawing();
	if ( nullptr != deviceContext ) {
		LoadResources( deviceContext );
		const D2D1_SIZE_F targetSize = deviceContext->GetSize();
		if ( ( targetSize.height > 0 ) && ( targetSize.height > 0 ) ) {
			UpdateBitmaps( m_LeftDisplayLevel, m_RightDisplayLevel );
			const float halfHeight = targetSize.height / ( m_IsStereo ? 2.0f : 1.0f );
			const D2D1_RECT_F leftRect = D2D1::RectF( 0, 0, targetSize.width, halfHeight );
			const D2D1_RECT_F rightRect = D2D1::RectF( 0, leftRect.bottom, targetSize.width, leftRect.bottom + halfHeight );
			const FLOAT opacity = 1.0;
			const D2D1_INTERPOLATION_MODE interpolationMode = IsHardwareAccelerationEnabled() ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_LINEAR;
			if ( nullptr != m_BitmapLeft ) {
				deviceContext->DrawBitmap( m_BitmapLeft, &leftRect, opacity, interpolationMode, NULL /*srcRect*/, NULL /*transform*/ );
			}
			if ( nullptr != m_BitmapRight ) {
				deviceContext->DrawBitmap( m_BitmapRight, &rightRect, opacity, interpolationMode, NULL /*srcRect*/, NULL /*transform*/ );
			}
			if ( nullptr != m_Brush ) {
				const float strokeWidth = s_RoundedCornerWidth * targetSize.width / VU_WIDTH;
				D2D1_ROUNDED_RECT roundedRect = {};
				roundedRect.radiusX = strokeWidth * 2;
				roundedRect.radiusY = strokeWidth * 2;
				roundedRect.rect = D2D1::RectF( -strokeWidth /*left*/, -strokeWidth /*top*/, targetSize.width + strokeWidth, targetSize.height + strokeWidth );
				deviceContext->DrawRoundedRectangle( roundedRect, m_Brush, strokeWidth * 2 );
			}
		}
		EndDrawing();
	}
}

void VUMeter::OnSettingsChange()
{
	m_Decay = GetSettings().GetVUMeterDecay();
	FreeResources();
}

void VUMeter::OnSysColorChange()
{
	FreeResources();
}

void VUMeter::LoadResources( ID2D1DeviceContext* deviceContext )
{
	if ( nullptr != deviceContext ) {

		if ( nullptr == m_Brush ) {
			deviceContext->CreateSolidColorBrush( D2D1::ColorF( GetSysColor( COLOR_3DFACE ) ), &m_Brush );
		}

		if ( ( nullptr == m_BitmapLeft ) && ( nullptr == m_BitmapRight ) ) {
			const D2D1_SIZE_U bitmapSize = D2D1::SizeU( VU_WIDTH, VU_HEIGHT );
			D2D1_BITMAP_PROPERTIES bitmapProperties = {};
			bitmapProperties.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE };
			HRESULT hr = deviceContext->CreateBitmap( bitmapSize, bitmapProperties, &m_BitmapLeft );
			if ( FAILED( hr ) ) {
				FreeResources();
			}
			if ( m_IsStereo ) {
				hr = deviceContext->CreateBitmap( bitmapSize, bitmapProperties, &m_BitmapRight );
				if ( FAILED( hr ) ) {
					FreeResources();
				}
			}

			DrawPin( 0 );
			const D2D1_RECT_U destRect = D2D1::RectU( 0 /*left*/, 0 /*top*/, VU_WIDTH /*right*/, VU_HEIGHT /*bottom*/ );
			const UINT32 pitch = VU_WIDTH * 4;
			if ( nullptr != m_BitmapLeft ) {
				m_BitmapLeft->CopyFromMemory( &destRect, m_MeterImage.data(), pitch );
			}
			if ( nullptr != m_BitmapRight ) {
				m_BitmapRight->CopyFromMemory( &destRect, m_MeterImage.data(), pitch );
			}
		}
	}
}

void VUMeter::FreeResources()
{
	if ( nullptr != m_BitmapLeft ) {
		m_BitmapLeft->Release();
		m_BitmapLeft = nullptr;
	}
	if ( nullptr != m_BitmapRight ) {
		m_BitmapRight->Release();
		m_BitmapRight = nullptr;
	}
	if ( nullptr != m_Brush ) {
		m_Brush->Release();
		m_Brush = nullptr;
	}
}

int VUMeter::GetPinPosition( const float level )
{
	return int( level * VU_PINCOUNT + 0.5f );
}
