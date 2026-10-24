#pragma once
/*
 * remote_log.h — log del esclavo replicado al maestro por ESP-NOW.
 *
 * Con REMOTE_LOG_ENABLE=1 el stream del log (DBG_STREAM) pasa a ser un "tee"
 * que escribe al Serial de siempre y ademas a un anillo; loop() saca lineas
 * del anillo y las manda al maestro (CMD_SLAVE_LOG), que las imprime en su
 * propio log con el prefijo del nodo. Sirve para depurar esclavos sin USB.
 * Si el anillo se llena se pierden las lineas mas viejas (se cuentan).
 */
#include <Arduino.h>

#ifndef REMOTE_LOG_ENABLE
#define REMOTE_LOG_ENABLE 0
#endif

#if REMOTE_LOG_ENABLE

class RemoteLogTee : public Print {
public:
    static constexpr size_t RING = 4096;

    void begin(unsigned long baud) { Serial.begin(baud); }
    size_t write(uint8_t c) override
    {
        Serial.write(c);
        push(&c, 1);
        return 1;
    }
    size_t write(const uint8_t *b, size_t n) override
    {
        Serial.write(b, n);
        push(b, n);
        return n;
    }
    /* Copia hasta max bytes terminando en el ultimo '\n' disponible (o max si
     * no hay ninguno y el anillo esta casi lleno). Devuelve los bytes sacados. */
    size_t take(char *out, size_t max)
    {
        size_t n = 0;
        portENTER_CRITICAL(&_mux);
        const size_t avail = (_head + RING - _tail) % RING;
        size_t lastNl = 0;
        for (size_t i = 0; i < avail && i < max; i++) {
            if (_buf[(_tail + i) % RING] == '\n') lastNl = i + 1;
        }
        /* Una linea mas larga que max no tiene '\n' en el tramo: se manda
         * partida (antes quedaba trabada hasta llenar medio anillo). */
        n = lastNl ? lastNl : (avail >= max ? max : 0);
        for (size_t i = 0; i < n; i++) out[i] = (char)_buf[(_tail + i) % RING];
        _tail = (_tail + n) % RING;
        portEXIT_CRITICAL(&_mux);
        return n;
    }
    uint32_t dropped() const { return _dropped; }

private:
    void push(const uint8_t *b, size_t n)
    {
        portENTER_CRITICAL(&_mux);
        for (size_t i = 0; i < n; i++) {
            const size_t next = (_head + 1) % RING;
            if (next == _tail) {   /* lleno: descartar el byte mas viejo */
                _tail = (_tail + 1) % RING;
                _dropped++;
            }
            _buf[_head] = b[i];
            _head = next;
        }
        portEXIT_CRITICAL(&_mux);
    }
    uint8_t  _buf[RING];
    volatile size_t _head = 0;
    volatile size_t _tail = 0;
    uint32_t _dropped = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

inline RemoteLogTee &remoteLogTee()
{
    static RemoteLogTee t;
    return t;
}

#endif  /* REMOTE_LOG_ENABLE */
