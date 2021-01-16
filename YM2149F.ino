#include <SPI.h>
#include <SD.h>

template<int SIZE, typename TYPE = uint8_t>
class RingBuffer
{
    TYPE m_buff[SIZE];
    int m_ri = 0;
    int m_wi = 0;
public:
    bool isEmpty() const
    {
    	return m_ri == m_wi;
    }
    int szData() const
    {
    	if (m_ri == m_wi)
    		return 0;
    	if (m_wi > m_ri)
    		return m_wi - m_ri;
    	return SIZE + m_wi - m_ri;
    }
    bool isFull() const
    {
    	return szData() == (SIZE - 1);
    }
    bool write(const TYPE data)
    {
    	if (isFull())
    		return false;
    	m_buff[m_wi] = data;
    	if (++m_wi == SIZE)
    		m_wi = 0;
    	return true;
    }
    bool read(TYPE& data)
    {
    	if (isEmpty())
    		return false;
    	data = m_buff[m_ri];
    	if (++m_ri == SIZE)
    		m_ri = 0;
    	return true;
    }
};

//
// DA0 - PC0 (pin 14 <A0>)
// DA1 - PC1 (pin 15 <A1>)
// DA2 - PC2 (pin 16 <A2>)
// DA3 - PC3 (pin 17 <A3>)
// DA4 - PD4 (pin 4)
// DA5 - PD5 (pin 5)
// DA6 - PD6 (pin 6)
// DA7 - PD7 (pin 7)
//
// BC1  - PB0 (pin 8)
// BDIR - PB1 (pin 9)
//
// RESET - PD2 (pin 2)
// CLOCK - PD3 (pin 3) OC2B Fast PWM
//
class AyPlayer
{	
    RingBuffer<200, uint8_t> m_buff;
    int m_count_delay = 0;    
	void _clock()
	{
		TCCR2A = 0x23;
		TCCR2B = 0x09;
		OCR2A = 8;
		OCR2B = 3;
	}
	void _reset()
	{
		PORTD = PIND & 0b11111011; // PD2 - LOW
		delay(100);
		PORTD = PIND | 0b00000100; // PD2 - HIGH
		delay(100);
	}
	void _cmd_nothing() // BC1 = 0, BDIR = 0
	{
    	PORTB = PINB & 0b11111100;		
	}
	void _cmd_fix_addr() // BC1 = 1, BDIR = 1
	{
		PORTB = PINB | 0b00000011;
	}
	void _cmd_fix_data() // BC1 = 1, BDIR = 0
	{
		PORTB = PINB | 0b00000010; 
	}
	void _out(uint8_t port, uint8_t data)
	{
		PORTC = (PINC & 0xF0) | (port & 0x0F);
		PORTD = (PIND & 0x0F);
		_cmd_fix_addr();
		delayMicroseconds(1);
		_cmd_nothing();
		PORTC = (PINC & 0xF0) | (data & 0x0F);
		PORTD = (PIND & 0x0F) | (data & 0xF0);
		_cmd_fix_data();
		delayMicroseconds(1);
		_cmd_nothing();
	}
	void _silent()
	{
		for (int i = 0; i < 16; i++) 
			_out(i, 0);
	}
public:
	AyPlayer() : m_count_delay(0)
	{	
	}
    void init()
    {
        DDRC = DDRC | 0b00001111; //out - PC3,PC2,PC1,PC0
        DDRD = DDRD | 0b11111100; //out - PD7,PD6,PD5,PD4,PD3,PD2
        DDRB = DDRB | 0b00000011; //out - PB1,PB0
        _clock();
        _reset();
        _silent();
    }
    void read2buff(File& f)
    {
        while(!m_buff.isFull())
        {
            if (!f.available()) 
                return;
            m_buff.write(f.read());
        }
    }
    void play20ms() // interrupt
    {
        if (m_count_delay)
        {
        	m_count_delay--;
        	return;
        }
        uint8_t data;
        while (m_buff.read(data))
        {
        	if (data == 0xFF)
        		return;
        	if (data == 0xFE)
        	{
        		if (m_buff.read(data))
	        		m_count_delay = 4 * data;
    			return;    				
        	}		
        	if (data == 0xFD)
        		break;
            uint8_t val;
            if (m_buff.read(val))
                _out(data, val);
        }        
		_silent();
    }
};

class SdReader
{
private:
    struct PSG_HEADER
    {
        char id[3]; //PSG
        char _1a;   //0x1a
        char ver;
        char freq;
        char data[10];
    };

	File m_root;
    File m_file;
	int m_countFiles = 0;
	
	int _get_count(File& dir)
	{
		int res = 0;
	    dir.rewindDirectory();
    	while (true)
    	{
			File entry = dir.openNextFile();
        	if (!entry) break;
	        if (!entry.isDirectory())
            	res++;
        	entry.close();
        }
        return res;
    }
    bool _prepare(const char* fname)
    {
        m_file.close();
        m_file = SD.open(fname);
        if (!m_file)
        {
            Serial.print("failed open \""); Serial.print(fname); Serial.println("\"");
            return false;
        }            
        PSG_HEADER hdr;
        if (m_file.read(&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.id[0] != 'P' || hdr.id[1] != 'S' || hdr.id[1] != 'G')
        {
            Serial.print("bad format \""); Serial.print(fname); Serial.println("\"");
            return false;
        }
        Serial.print("file \""); Serial.print(fname); Serial.println("\" opened");
        return true;
    }

public:

    bool init()
    {
        pinMode(10, OUTPUT);
        digitalWrite(10, HIGH);
        if (!SD.begin(10))
        {
            Serial.println("sd card initialization failed");
            return false;
        }
        m_root = SD.open("/");
        if (!m_root)
        {
            Serial.println("sd root read failed");
            return false;
        }
        m_countFiles = _get_count(m_root);
        Serial.print(m_countFiles);
        Serial.println(" files found");
        return true;
    }    
    bool openRandomFile()
    {
        int index = random(m_countFiles);
        m_root.rewindDirectory();
        int i = 0;
        while (true)
        {
            File entry = m_root.openNextFile();
            if (!entry) break;
            if (!entry.isDirectory())
            {
                if (i == index)
                {
                    bool ok = _prepare(entry.name());
                    entry.close();
                    return ok;
                }
                i++;
            }
            entry.close();
        }
        return false;
    }
    File& file()
    {
        return m_file;
    }
};

void setupTimer()
{
    cli();
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1 = 0;
    OCR1A = 1250;
    TCCR1B |= (1 << WGM12);
    TCCR1B |= (1 << CS12);
    TIMSK1 |= (1 << OCIE1A);
    sei();
}

AyPlayer ay;
SdReader sd;

bool is_work = false;

void setup()
{
    Serial.begin(9600);
    randomSeed(analogRead(4) + analogRead(5));
    ay.init();
    if (sd.init())
    {
        if (sd.openRandomFile())
        {
            ay.read2buff(sd.file());
            setupTimer();
            is_work = true;
        }
    }
}

void loop()
{
    if (!sd.file().available())
        if (!sd.openRandomFile()) 
            return;
    ay.read2buff(sd.file());
}

ISR(TIMER1_COMPA_vect)
{
    ay.play20ms();
}
