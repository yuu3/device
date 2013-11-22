#include "Keys.h"
#include "Arduino.h"
#include <avr/eeprom.h>
#include "pgmStrToRAM.h"
#include "CRC8.h"
#include "Global.h"
#include "convert.h"

#define MORSE_CREDENTIALS_SEPARATOR '/'

//
// DESCRIPTION:
// Fill wifi credentials
//

KeysFiller::KeysFiller ()
    : state(KeysFillerStateSecurity)
{
}

//
// DESCRIPTION:
// Save wifi credentials in EEPROM
//

Keys::Keys()
{
    data = (KeysShared*)global.buffer;
}

void Keys::load()
{
    eeprom_read_block((void*)data,   (void*)0,                  sizeof(KeysShared));
    eeprom_read_block((void*)&data2, (void*)sizeof(KeysShared), sizeof(KeysIndependent));
    Serial.println(sizeof(KeysShared));
    Serial.println(sizeof(KeysIndependent));
    if (! isCRCOK()) {
        clear();
    }
}

bool Keys::isCRCOK()
{
    uint8_t crc = crc8( (uint8_t*)data, sizeof(KeysCRCed) );
    return (crc == data->crc8);
}

// crc8 is ok and wifi credentials are valid
bool Keys::isWifiCredentialsSet()
{
    return data->wifi_is_set;
}

bool Keys::isAPIKeySet()
{
    return data2.key_is_set;
}

bool Keys::isValid()
{
    return data2.key_is_set && data2.key_is_valid;
}

bool Keys::wasWifiValid()
{
    return data->wifi_was_valid;
}

GSSECURITY Keys::getSecurity()
{
    return (GSSECURITY)data->security;
}

const char* Keys::getSSID()
{
    return data->ssid;
}

const char* Keys::getPassword()
{
    return data->password;
}

const char* Keys::getKey()
{
    return data2.key;
}

void Keys::set(GSSECURITY security, const char *ssid, const char *pass)
{
    data->security = security;
    strcpy(data->ssid,     ssid);
    strcpy(data->password, pass);
    data->wifi_is_set = true;
}

void Keys::setKey(const char *key)
{
    strcpy(data2.key, key);
    data2.key_is_set = true;
}

void Keys::setWifiWasValid(bool valid)
{
    data->wifi_was_valid = valid;
}

void Keys::setKeyValid(bool valid)
{
    data2.key_is_valid = valid;
}

void Keys::save(void)
{
    data->crc8    = crc8( (uint8_t*)data, sizeof(KeysCRCed) );
    eeprom_write_block((const void*)data,   (void*)0,                  sizeof(KeysShared));
    save2();
}

void Keys::save2(void)
{
    eeprom_write_block((const void*)&data2, (void*)sizeof(KeysShared), sizeof(KeysIndependent));
}

void Keys::clear(void)
{
    memset( data, 0, sizeof(KeysShared) );

    clearKey();

    save();

    filler.state = KeysFillerStateSecurity;
    filler.index = 0;
}

void Keys::clearKey(void)
{
    memset( &data2, 0, sizeof(KeysIndependent) );
}

// we use morse code to transfer Security, SSID, Password, Key, CRC8 to IRKit device
// SSID can be multi byte, so we transfer HEX 4bit as 1 ASCII character (0-9A-F),
// so we need 2 morse letters to transfer a single character.
// [0248]/#{SSID}/#{Password}/#{Key}/#{CRC}
int8_t Keys::put(char code)
{
    static uint8_t character;
    static bool is_first_byte;

    if (code == MORSE_CREDENTIALS_SEPARATOR) {
        // wait for putDone() on CRC state
        switch (filler.state) {
        case KeysFillerStateSSID:
            data->ssid[ filler.index ] = 0;
            break;
        case KeysFillerStatePassword:
            data->password[ filler.index ] = 0;
            break;
        case KeysFillerStateKey:
            data->temp_key[ filler.index ] = 0;
            break;
        default:
            break;
        }
        if (filler.state != KeysFillerStateCRC) {
            filler.state = (KeysFillerState)( filler.state + 1 );
        }
        is_first_byte = 1;
        filler.index  = 0;
        return 0;
    }
    if ( ! (('0' <= code) && (code <= '9')) &&
         ! (('A' <= code) && (code <= 'F')) ) {
        // we only use letters which match: [0-9A-F,]
        Serial.print(("!E23:")); Serial.println( code, HEX );
        return -1;
    }
    if (filler.state == KeysFillerStateSecurity) {
        switch (code) {
        // we try OPEN when WEP failed, to present less options to user
        case '0': // GSwifi::GSSECURITY_NONE:
        // case '1': // GSwifi::GSSECURITY_OPEN:
        case '2': // GSwifi::GSSECURITY_WEP:
        case '4': // GSwifi::GSSECURITY_WPA_PSK:
        case '8': // GSwifi::GSSECURITY_WPA2_PSK:
            data->security = (GSSECURITY)x2i(code);
            return 0;
        default:
            Serial.print(("!E22:")); Serial.println( code, HEX );
            return -1;
        }
    }
    // we transfer [0..9A..F] as ASCII
    // so 2 bytes construct 1 character, network *bit* order
    if (is_first_byte) {
        character          = x2i(code);
        character        <<= 4;         // F0h
        is_first_byte   = false;
        return 0;
    }
    else {
        character        += x2i(code); // 0Fh
        is_first_byte  = true;
    }

    switch (filler.state) {
    case KeysFillerStateSSID:
        if ( filler.index == MAX_WIFI_SSID_LENGTH ) {
            Serial.println(("!E18"));
            return -1;
        }
        data->ssid[ filler.index ++ ] = character;
        break;
    case KeysFillerStatePassword:
        if ( filler.index == MAX_WIFI_PASSWORD_LENGTH ) {
            Serial.println(("!E19"));
            return -1;
        }
        data->password[ filler.index ++ ] = character;
        break;
    case KeysFillerStateKey:
        if (filler.index == MAX_KEY_LENGTH) {
            Serial.println(("!E20"));
            return -1;
        }
        data->temp_key[ filler.index ++ ] = character;
        break;
    case KeysFillerStateCRC:
        if (filler.index > 0) {
            Serial.println(("!E21"));
            return -1;
        }
        data->crc8 = character;
        filler.index ++;
        break;
    default:
        return -1;
    }
    return 0;
}

int8_t Keys::putDone()
{
    if (filler.state != KeysFillerStateCRC) {
        Serial.println(("!E17"));
        return -1;
    }

    data->wifi_is_set = true;

    if (isCRCOK()) {
        // copy to data2 area
        strcpy(data2.key, data->temp_key);
        data2.key_is_set   = true;
        data2.key_is_valid = false;
        return 0;
    }
    else {
        dump();
        Serial.println(("!E16"));
        return -1;
    }
}

void Keys::dump(void)
{
    Serial.print(("F:"));
    Serial.println(data->wifi_is_set);
    Serial.println(data->wifi_was_valid);
    Serial.println(data2.key_is_set);
    Serial.println(data2.key_is_valid);

    Serial.print(("E:"));
    Serial.println(data->security);

    Serial.print(("S:"));
    Serial.println((const char*)data->ssid);
    Serial.println((const char*)data->password);
    Serial.println((const char*)data2.key);
    Serial.println(data->crc8, HEX);
}
