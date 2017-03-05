// 
// SEM EPS for Arduino Due
// Input: Analog in A0, A1, A2, A3, hsync and vsync at D3 and D4
// Output: USB frames to PC
//




// USB communication headers

#define COMMAND_BYTES 16
byte headerConnect[COMMAND_BYTES]  = {'E','P','S','_','S','E','M','_','C','O','N','N','E','C','T','.'};
byte headerReady[COMMAND_BYTES]    = {'E','P','S','_','S','E','M','_','R','E','A','D','Y','.','.','.'};
byte headerFrame[COMMAND_BYTES]    = {'E','P','S','_','S','E','M','_','F','R','A','M','E','.','.','.'};
byte headerBytes[COMMAND_BYTES]    = {'E','P','S','_','S','E','M','_','B','Y','T','E','S','.','.','.'};
byte headerEndFrame[COMMAND_BYTES] = {'E','P','S','_','S','E','M','_','E','N','D','F','R','A','M','E'};
byte headerReset[COMMAND_BYTES]    = {'E','P','S','_','S','E','M','_','R','E','S','E','T','.','.','.'};
byte headerIdle[COMMAND_BYTES]     = {'E','P','S','_','S','E','M','_','I','D','L','E','.','.','.','.'};

#define SENTINEL_BYTES 16
byte sentinelTrailer[SENTINEL_BYTES] = {0,1,2,3,4,5,6,7,8,9,0xA,0xB,0xC,0xD,0xE,0xF};

struct BytesParams {
  byte      headerBytes[16];
  uint32_t  checkSum;         // now includes everything from here on, to just before sentinel trailer
  uint16_t  line;
  uint16_t  bytes;
};

struct BytesParams *g_pbp = NULL;


//
// resolution recognition (autosync) structures and array
//

struct Resolution {
  int scanLineTime; // scan length (in microseconds) for recognizing this resolution
  int numPixels;    // number of pixels we will set up to scan for line
  int numChannels;  // number of channels for every pixel
  int numLines;     // documented number of lines (in actuality, we will obey vsync)
  int preScaler;    // prescale factor for Arduino Due ADC; ADC clock is slowed by factor (prescale+1)*2 
};

struct Resolution *g_pCurrentRes;


// resolutions are stored in this array in ascending order of horizontal scan times
struct Resolution g_allRes[] = {
// scan line time, pixels, channels, spec lines, prescaler
  {           160,    200,        1,        182,         0 }, // RAPID2 mag 100+
  {          1183,   1200,        1,        536,         0 }, // RAPID2 mag 10
  {          5790,   3540,        1,        864,         2 }, // SLOW1
  {         10561,  10500,        1,       4500,         1 }, // H3V7 at 9000x
  {         11070,  10500,        1,       4500,         1 }, // H3V7 at 10x
  {         33326,   9440,        1,       3000,         5 }  // H6V7
};

#define NUM_MODES (sizeof(g_allRes)/sizeof(struct Resolution))




#define VSYNC_PIN   2
#define HSYNC_PIN   3

#define PHASE_IDLE                0
#define PHASE_READY_TO_MEASURE    1
#define PHASE_MEASURING           2
#define PHASE_READY_FOR_SCAN      3
#define PHASE_SCANNING            4
#define PHASE_CHECK               5

#define REASON_IDLE   0
#define REASON_NO_RES 1
#define REASON_TRACK  2
#define REASON_VSYNC  3

int g_channelSelection1 = 0; // TODO: Make this programmable with digital inputs
int g_channelSelection2 = 3; // TODO: For now, make sure that SEI is on A0 and AEI on A1

//
// Buffers are really independent of the resolution we are tracking, just make them big enough
//

#define NUM_BUFFERS   2 // fill one with DMA while main program reads and copies the other
#define BUFFER_LENGTH (5000 * 8 + 64)
#define BUFFER_BYTES  (BUFFER_LENGTH * sizeof(uint16_t))
#define NEXT_BUFFER(n)((n+1)%NUM_BUFFERS) // little macro to aid switch to next buffer

volatile int currentBuffer; 
volatile int nextBuffer;
uint16_t *padcBuffer[NUM_BUFFERS];   
volatile unsigned long g_adcLineTimeStart;
volatile unsigned long g_adcLineTime;
volatile bool g_adcInProgress;
int g_lineBytes;
volatile int g_phase = PHASE_CHECK;
volatile int g_measuredLineTime;
volatile long g_trackTimeStart;
volatile long g_prevTrackTimeStart;
volatile long g_trackTime;
volatile long g_trackFaults;
volatile int g_reason = REASON_IDLE;
volatile int g_argument;
volatile int g_numLines;
volatile int g_resFaults;


#define MAX_RES_FAULTS 10
#define MAX_TRACK_FAULTS 10
#define MAX_ERRORS  100
#define USB_TIMEOUT 100
#define USB_MIN_WRITE_BUFFER_SIZE 60









// code to blink the built-in LED n times 
const int builtInLEDPin = 13; 

void blinkBuiltInLED(int n) {
  
  for (int i= 0; i<n; i++) {
    // blink built-in LED
    digitalWrite(builtInLEDPin, HIGH);
    delay(200);
    digitalWrite(builtInLEDPin, LOW);
    if(i < n-1) {
      delay(200);
    }
  }
}


//
// ERROR HALT
// 

void halt(int blinks) {
  while (true) {
    blinkBuiltInLED(blinks);
    delay(1000);
  }
}

//
// software reset
//

void reset() {

  detachInterrupt(VSYNC_PIN); 
  detachInterrupt(HSYNC_PIN); 

  freeLineBuffers(); // safe to do
  
  while(SerialUSB.available()) {
    SerialUSB.read();
  }
  delay(100);

  setup();
}

bool g_fIRQ = false;

void setup() {
  // start USB
  SerialUSB.begin(0); 
  SerialUSB.write(headerReset, 16);

  // set up built-in blink LED, custom led, pushButton
  pinMode (builtInLEDPin, OUTPUT);
  // visual signal that we are alive
  blinkBuiltInLED(1);
  
  int n;
  byte buffer[16];

  // initialize buffer pointers to NULL
  g_pbp = NULL;
  for (int i = 0; i < NUM_BUFFERS; i++) {
    padcBuffer[i] = NULL;
  }

 
  // wait for USB connect command from host
  do {
    do {
      // wait for any request
      do {
        delayMicroseconds(20); 
        n = SerialUSB.available();
      } while (n == 0);


      // read the request
      SerialUSB.readBytes(buffer, 16);

      // read any extra bytes
      while (SerialUSB.available()) {
        SerialUSB.read();   
      }
    } while (n != COMMAND_BYTES);
   } while (memcmp(buffer, headerConnect, COMMAND_BYTES) != 0);
  
  // acknowledge connection request
  SerialUSB.write(headerReady, 16);
  SerialUSB.flush();
  blinkBuiltInLED(2);  
  g_pCurrentRes = NULL;
  g_phase = PHASE_CHECK;
  g_reason = REASON_IDLE;

  setupInterrupts();
}



void computeCheckSum(int line, int bytes) {
  // compute checkSum
  long checkSum = 0;
  uint16_t *pWord = (uint16_t *)&g_pbp[1];
  for (int i=0; i<bytes/2; i++) {
    checkSum += *pWord++;
  }
  
  g_pbp->checkSum = checkSum + line + bytes;
  g_pbp->line = line;
  g_pbp->bytes = bytes;
}


bool sendLine(int bytes) {
  SerialUSB.write((uint8_t *)g_pbp, sizeof(struct BytesParams) +  bytes + sizeof(sentinelTrailer));
  return true;
}



int setupLineBuffers() {
  // compute raw byte number for one scan line
  int bytes = (g_pCurrentRes->numPixels * g_pCurrentRes->numChannels * sizeof(uint16_t));

  for (int i = 0; i < NUM_BUFFERS; i++) {
    padcBuffer[i] = (uint16_t *)malloc(bytes);
    if (padcBuffer[i] == NULL) {
      halt(3);
    }
  }
  
  // compute buffer size for whole USB command, allocate, and fill with known info
  int bufferSize = sizeof(struct BytesParams) +  bytes + sizeof(sentinelTrailer);
  g_pbp = (struct BytesParams *) malloc (bufferSize); 
  if (g_pbp == NULL) {
    halt(4);
  }

  memcpy(&g_pbp->headerBytes, headerBytes, sizeof(headerBytes));
  memcpy(((byte *)&g_pbp[1]) + bytes, sentinelTrailer, sizeof (sentinelTrailer)); 
  g_pbp->bytes = bytes;

  // return the size for USB send
  return bufferSize;
}




void freeLineBuffers() {
  for (int i = NUM_BUFFERS-1; i >= 0; i--) {
    if (padcBuffer != NULL) {
      free(padcBuffer[i]);
      padcBuffer[i] = NULL;
    }
  }

  if (g_pbp != NULL) {
    free (g_pbp);
    g_pbp = NULL;
  }
}





void sendFrameHeader() {
  flipLED();
  SerialUSB.write(headerFrame, 16);                     // send FRAME header
  SerialUSB_write_uint16_t(g_pCurrentRes->numChannels); // number of channels                       
  SerialUSB_write_uint16_t(g_pCurrentRes->numPixels);   // width in pixels
  SerialUSB_write_uint16_t(g_pCurrentRes->numLines);    // height in lines
  SerialUSB_write_uint16_t(g_measuredLineTime);         // measured line scan time that determined resolution
  
  switch (g_pCurrentRes->numChannels) {
    case 4:
      SerialUSB_write_uint16_t(0);
      SerialUSB_write_uint16_t(1);
      SerialUSB_write_uint16_t(2);
      SerialUSB_write_uint16_t(3);
      break;

    case 2:
      // list just the specific 2 channels we are capturing
      SerialUSB_write_uint16_t(g_channelSelection1);
      SerialUSB_write_uint16_t(g_channelSelection2);
      SerialUSB_write_uint16_t(255);
      SerialUSB_write_uint16_t(255);
      break;

    case 1:
      // list just the specific 1 channel we are capturing in FAST
      SerialUSB_write_uint16_t(g_channelSelection1);
      SerialUSB_write_uint16_t(255);
      SerialUSB_write_uint16_t(255);
      SerialUSB_write_uint16_t(255);
      break;
  }
}

void sendEndFrame(int lineTime, int frameTime) {
  delayMicroseconds(lineTime / 2);  
  SerialUSB.write(headerEndFrame, 16);     // send EFRAME (end frame), line time, frame time
  SerialUSB_write_uint16_t((uint16_t)lineTime);
  SerialUSB_write_uint16_t((uint16_t)frameTime);
  delayMicroseconds(lineTime / 2);  
}

  





void SerialUSB_write_uint16_t(uint16_t word) {
    SerialUSB.write((byte)(word & 255));
    SerialUSB.write((byte)(word >> 8));
}

void SerialUSB_write_uint32_t(uint32_t word) {
    SerialUSB.write((byte)(word & 255));                           
    SerialUSB.write((byte)((word >> 8) & 255));
    SerialUSB.write((byte)((word >> 16) & 255));
    SerialUSB.write((byte)(word >> 24));
}


void adcConfigureGain() {
  adc_enable_anch(ADC); 
  
  adc_set_channel_input_gain(ADC, (adc_channel_num_t)(g_APinDescription[0].ulADCChannelNumber), ADC_GAINVALUE_0);
  adc_set_channel_input_gain(ADC, (adc_channel_num_t)(g_APinDescription[1].ulADCChannelNumber), ADC_GAINVALUE_0);
  adc_set_channel_input_gain(ADC, (adc_channel_num_t)(g_APinDescription[2].ulADCChannelNumber), ADC_GAINVALUE_0);
  adc_set_channel_input_gain(ADC, (adc_channel_num_t)(g_APinDescription[3].ulADCChannelNumber), ADC_GAINVALUE_0);
  
  adc_disable_channel_input_offset(ADC, (adc_channel_num_t)(g_APinDescription[0].ulADCChannelNumber));
  adc_disable_channel_input_offset(ADC, (adc_channel_num_t)(g_APinDescription[1].ulADCChannelNumber));
  adc_disable_channel_input_offset(ADC, (adc_channel_num_t)(g_APinDescription[2].ulADCChannelNumber));
  adc_disable_channel_input_offset(ADC, (adc_channel_num_t)(g_APinDescription[3].ulADCChannelNumber));
}




//
// set up analog-to-digital conversions
//

void initializeADC() {
  // convert from Ax input pin numbers to ADC channel numbers
  int channel1 = 7-g_channelSelection1;
  int channel2 = 7-g_channelSelection2;

  pmc_enable_periph_clk(ID_ADC);
  adc_init(ADC, SystemCoreClock, ADC_FREQ_MAX, ADC_STARTUP_FAST);
  analogReadResolution(12);
  adcConfigureGain();

  ADC->ADC_MR &= 0xFFFF0000;     // mode register "prescale" zeroed out. 
  ADC->ADC_MR |= 0x80000000;     // high bit indicates to use sequence numbers
  ADC->ADC_MR |= g_pCurrentRes->preScaler << 8;     
  ADC->ADC_EMR |= (1<<24);      // turn on channel numbers
  ADC->ADC_CHDR = 0xFFFFFFFF;   // disable all channels   

  switch (g_pCurrentRes->numChannels) {
    case 4:
    // set 4 channels 
    ADC->ADC_CHER = 0xF0;         // enable ch 7, 6, 5, 4 -> pins a0, a1, a2, a3
    ADC->ADC_SEQR1 = 0x45670000;  // produce these channel readings for every completion
    break;
    
    case 2:
    // set 2 channels  
    ADC->ADC_CHER = (1 << channel1) | (1 << channel2);
    ADC->ADC_SEQR1 = (channel1 << (channel2 *4)) | (channel2 << (channel1*4));
    break;

    case 1:
    //todo: make sure this works
    ADC->ADC_CHER = (1 << channel1); // todo: does this work for channels other than A0?
    ADC->ADC_SEQR1 = (channel1 << (channel1 *4));
    // set 2 channels (same channel) 
//    ADC->ADC_CHER = (1 << channel1) | (1 << channel2);
//    ADC->ADC_SEQR1 = (channel1 << (channel2 *4)) | (channel1 << (channel1*4));
    break;
  }

  NVIC_EnableIRQ(ADC_IRQn);

  //ADC->ADC_IDR = ~(1 << 27);              // disable other interrupts
  ADC->ADC_IER = 1 << 27;                 // enable the DMA one
  ADC->ADC_RPR = (uint32_t)padcBuffer[0]; // set up DMA buffer
  ADC->ADC_RCR = g_pCurrentRes->numPixels * g_pCurrentRes->numChannels;           // and number of words
  ADC->ADC_RNPR = (uint32_t)padcBuffer[1]; // next DMA buffer
  ADC->ADC_RNCR = g_pCurrentRes->numPixels * g_pCurrentRes->numChannels;  

  currentBuffer = 0;
  nextBuffer = 1; 

  ADC->ADC_PTCR = 1;
  ADC->ADC_CR = 2;

  g_adcInProgress = false;
}

void ADC_Handler() {
  // move DMA pointers to next buffer

  int flags = ADC->ADC_ISR;                           // read interrupt register
  if (flags & (1 << 27)) {                            // if this was a completed DMA
    stopADC();
    nextBuffer = NEXT_BUFFER(nextBuffer);             // get the next buffer (and let the main program know)
    ADC->ADC_RNPR = (uint32_t)padcBuffer[nextBuffer]; // put it in place
    ADC->ADC_RNCR = g_pCurrentRes->numPixels * g_pCurrentRes->numChannels;
    g_adcInProgress = false;
    }
}

void stopADC() {
    ADC->ADC_MR &=0xFFFFFF00;                         // disable free run mode
    g_adcLineTime = micros() - g_adcLineTimeStart;    // record microseconds
}

void startADC() {
  if (!g_adcInProgress) {
    switch (g_pCurrentRes->numChannels) {
      case 4:
        ADC->ADC_MR |=0x000000F0;     // a0-a3 free running
        break;
        
      case 2:
        ADC->ADC_MR |= (1<<(7-g_channelSelection1)) | (1<<(7-g_channelSelection2));     // two channels free running
        break;
        
      case 1:
        ADC->ADC_MR |= (1<<(7-g_channelSelection1));     // one channel free running
        break;
    }
  g_adcInProgress = true;
  g_adcLineTimeStart = micros();
  }
 
}


void setupInterrupts() {
  pinMode(VSYNC_PIN, INPUT);
  pinMode(HSYNC_PIN, INPUT);
  attachInterrupt(VSYNC_PIN, vsyncHandler, FALLING);  // catch falling edge of vsync to get ready for measuring
  attachInterrupt(HSYNC_PIN, hsyncHandler, RISING);  // catch rising edge of hsync to start ADC
}


void flipLED() {
  volatile static bool fOn = false;

    if (fOn) {
      digitalWrite(LED_BUILTIN, LOW);
      fOn = false;
    } else {
      digitalWrite(LED_BUILTIN, HIGH);
      fOn = true;
    }
}

void vsyncHandler() {
//  flipLED();
  if (digitalRead(VSYNC_PIN) == LOW) { 
   
    switch (g_phase) {
      case PHASE_IDLE:
        g_phase = PHASE_READY_TO_MEASURE;
        g_resFaults = 0;
        break;
        
      case PHASE_SCANNING:
        // time to end the frame and send the image
        g_reason = REASON_VSYNC; 
        g_phase = PHASE_IDLE;
        break;
  
      case PHASE_READY_TO_MEASURE:
      case PHASE_MEASURING:
      case PHASE_READY_FOR_SCAN:
        // we should never get here, but hey, just stop everything
        g_phase = PHASE_CHECK;
        break;
    }
  }
}

void hsyncHandler() {
  if (digitalRead(HSYNC_PIN) == HIGH) {
    switch (g_phase) {
      case PHASE_IDLE:
        // not doing anything right now
        break;
      
      case PHASE_READY_TO_MEASURE:
        // start stopwatch, switch phase
        g_measuredLineTime = micros();
        g_phase = PHASE_MEASURING;
        break;
  
      case PHASE_MEASURING:
        // take scan time, get ready to start scanning (initiated by main program, need to set up ADC first)
        g_measuredLineTime = micros() - g_measuredLineTime;
        g_phase = PHASE_READY_FOR_SCAN;
        g_trackTimeStart = 0;
        g_numLines = 0;
        break;
        
      case PHASE_SCANNING:
        // keep track of hsync interval, if resolution changes, main routine will trigger vsync and end frame
        g_prevTrackTimeStart = g_trackTimeStart;
        g_trackTimeStart = micros();
        if (g_prevTrackTimeStart != 0) {
          g_trackTime = g_trackTimeStart - g_prevTrackTimeStart;
        }
        
        // start ADC (completion handled by ADC interrupt)
        startADC();
        ++g_numLines;
        break;
    }
  }
}


//
// takes line scan time in us, returns pointer to resolution info, or null if not recognized
//

struct Resolution *getResolution(int lineTime) {
  static struct Resolution customRes;
  int i;

  // if currently in autosync custom res, try to stay in it
  if (g_pCurrentRes == &customRes) {
    if ((lineTime < (customRes.scanLineTime + 20)) && (lineTime  > (customRes.scanLineTime - 20)))
      return &customRes;
  }

  // go through known resolutions
  
  for (i=0; i<NUM_MODES; i++) {
    if (lineTime > (g_allRes[i].scanLineTime - 100) && lineTime < (g_allRes[i].scanLineTime + 100)) {
      return &g_allRes[i];
    }
  }

  // autosync
// todo: not working yet
return NULL;

  if (lineTime > 150 && lineTime < 50000) {
    /*
     scan line time, pixels, channels, spec lines, prescaler
    {           160,    260,        1,        266,         0 }, // RAPID2 1.77 samples/us
    {          5790,   1920,        2,        864,         2 }, // SLOW1  0.67 samples/us
    {         33326,   2760,        4,       3000,         5 }  // H6V7   0.33 samples/us
    */
    // suppose we had around 12kb to play with
    long maxWords = 6000;
    long maxLineSamples = (long) ((double)lineTime * (double) 1.77); 

    customRes.scanLineTime = lineTime;        // that one was easy
    customRes.numChannels  = 1;               // also easy
    customRes.numLines     = 200;             // complete guess
    customRes.preScaler    = 1;               // this is where we start
    customRes.numPixels    = maxLineSamples;  // what we wish for
    
    while (customRes.numPixels > maxWords) {
      customRes.preScaler++;
      customRes.numPixels /= 2;
    }

    customRes.numPixels &= (~7); // round down to nearest 8
    customRes.preScaler--; // encoded: - 1
    return &customRes;
  }

  // nothing to be done
  return NULL;
}


bool okToWrite() {
  return (g_measuredLineTime > 4500) || (SerialUSB.availableForWrite() > USB_MIN_WRITE_BUFFER_SIZE);
}



void loop () {
  static int timeFrame = 0;
  static bool fFrameInProgress = false;
  static struct Resolution *lastRes = NULL;
  static int dropCount = 0;

  int timeLineScan = 0;
  char o,k;

  // 
  // let hsync measure the time
  //
  
  while (g_phase == PHASE_READY_TO_MEASURE) {
  }

  while (g_phase == PHASE_MEASURING) {
  }

  //
  // time has been measured by hsync, let's do our calculations, set up the scan buffers and start scanning!
  //
  if (g_phase == PHASE_READY_FOR_SCAN) {
    g_pCurrentRes = getResolution(g_measuredLineTime);
    if (g_pCurrentRes != NULL) {
      if (g_pCurrentRes != lastRes) {
        adjustToNewRes();
        lastRes = g_pCurrentRes;
      }

      // if lowres, make sure the queue has room. if hires, we have time to block on the write
      if (okToWrite()) {
        fFrameInProgress = true;
        sendFrameHeader();
  
        timeFrame = millis();
        g_trackTime = g_measuredLineTime;
        g_trackFaults = 0;
        dropCount = 0;
        g_phase = PHASE_SCANNING;
      } else {
        g_phase = PHASE_CHECK;
      }
    } else {// res not recognized
      if (g_resFaults++ > MAX_RES_FAULTS) {
        g_reason = REASON_NO_RES;
        g_argument = g_measuredLineTime;
        g_phase = PHASE_IDLE;
      } else
        g_phase = PHASE_READY_TO_MEASURE;
    }    
  }

  //
  // main line scanning
  // vsync will get us out of this
  //
  while (g_phase == PHASE_SCANNING) {
    // drop the first few lines
    if (dropCount > 0) {
      --dropCount;
      break;
    }
    // wait for scan completion, get line out of the way of the DMA controller
    scanAndCopyOneLine();

    // keep track of maximum scan time (we report this so we can dial it in and get max possible pixel res)
    timeLineScan = max(timeLineScan, g_adcLineTime);

    // compute check sum and fill in line and bytes
    computeCheckSum(g_numLines, g_lineBytes);

    // try to send the line, if things go wrong too often, reset.
    if (okToWrite()) {
      if (!sendLine(g_lineBytes)) {
        reset();
        return;
      }
    }
           
    // if resolution changed, end the frame by switching to next phase
    if ((g_trackTime > (g_measuredLineTime + 50)) || (g_trackTime < (g_measuredLineTime - 50))) {
      if (++g_trackFaults >= MAX_TRACK_FAULTS){
        g_reason = REASON_TRACK;
        g_argument = g_trackTime;
        g_phase = PHASE_IDLE;
      }
    }
  }

  //
  // aftermath
  // send frame, check for abort
  //
  if (g_phase == PHASE_IDLE) {
    if (fFrameInProgress) {
      timeFrame = millis() - timeFrame;
      sendEndFrame (timeLineScan, g_reason);
      fFrameInProgress = false;
    } else {
      if (okToWrite()) {
        // reasons
        // 0 idle
        // 1 no res
        // 2 changed res
        // 3 other vsync
        sendIdle(g_reason, g_argument);
      
      }
    }
    fFrameInProgress = false;
    g_phase = PHASE_CHECK;
  }

  if (g_phase == PHASE_CHECK) {
    // check for abort
    o = 0;
    while (SerialUSB.available()) {
      k = SerialUSB.read();
      if (o == 'A' && k == 'B') {
        reset();
        return;
      } 
      o = k;
    }
    g_phase = PHASE_READY_TO_MEASURE;
  }

  
}

void adjustToNewRes() {
    // free previous buffers (safe to do)
    freeLineBuffers();

    //
    // calculate some basic frame parameters and allocate buffer, init adc
    //
    g_lineBytes = (g_pCurrentRes->numPixels * g_pCurrentRes->numChannels * sizeof(uint16_t));
    setupLineBuffers();
    initializeADC();
}



void scanAndCopyOneLine() {
  while (NEXT_BUFFER(currentBuffer) == nextBuffer) {                  // while current and next are one apart
  }

  // put the line somewhere safe from adc, just past the params header:
  memcpy(&g_pbp[1], padcBuffer[currentBuffer], g_lineBytes);
  currentBuffer = NEXT_BUFFER(currentBuffer);                         // set next buffer for waiting
}


void sendIdle(int reason, int argument) {
    SerialUSB.write(headerIdle, 16);
    SerialUSB_write_uint32_t(reason);
    SerialUSB_write_uint32_t(argument);
}




