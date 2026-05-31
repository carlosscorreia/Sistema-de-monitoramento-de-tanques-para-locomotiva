#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <SPI.h>
#include <SD.h>

// ======================================================
// LCD I2C
// ======================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ======================================================
// RTC
// ======================================================

RTC_DS1307 rtc;

// ======================================================
// SD CARD
// ======================================================

const int chipSelect = 53;

// Nome no formato 8.3 (maximo 8 caracteres antes do ponto).
// "historico" tem 9 letras e pode falhar na biblioteca SD padrao.
const char *ARQUIVO_LOG = "historic.txt";

// ======================================================
// SENSORES ULTRASSONICOS
// ======================================================

// Combustivel
#define TRIG_COMB 22
#define ECHO_COMB 23

// Agua
#define TRIG_AGUA 24
#define ECHO_AGUA 25

// Oleo
#define TRIG_OLEO 26
#define ECHO_OLEO 27

// ======================================================
// LEDs COMBUSTIVEL
// ======================================================

#define LED_VERDE      30
#define LED_AMARELO    31
#define LED_VERMELHO   32

// ======================================================
// LEDs ALERTA MANUTENCAO
// ======================================================

#define LED_AGUA   33
#define LED_OLEO   34

// ======================================================
// BUZZER E RELE
// ======================================================

#define BUZZER 35
#define RELE   36

// Muitos modulos rele de Arduino sao acionados em nivel BAIXO (active-LOW).
// Se o seu rele ligar invertido na pratica, troque para true.
const bool RELE_ATIVO_BAIXO = false;

// ======================================================
// CONFIGURACOES DOS TANQUES (altura em cm)
// ======================================================

float alturaTanqueComb = 30.0;
float alturaTanqueAgua = 25.0;
float alturaTanqueOleo = 20.0;

// ======================================================
// LIMIARES (%)
// ======================================================

const float COMB_CRITICO    = 30.0;   // abaixo: alarme + rele
const float COMB_ATENCAO    = 60.0;   // entre 30 e 60: amarelo
const float NIVEL_MIN_MANUT = 80.0;   // agua e oleo

// Valor retornado quando o sensor falha (sem eco / desconectado)
const float ERRO_SENSOR = -1.0;

// ======================================================
// VARIAVEIS
// ======================================================

unsigned long ultimoRegistro = 0;

const unsigned long intervaloSD = 10000;

// ======================================================
// FUNCAO MEDIA MOVEL (com protecao contra leitura invalida)
// ======================================================

float medirNivel(int trigPin, int echoPin, float alturaTanque)
{
    const int numLeituras = 10;

    float soma = 0;
    int leiturasValidas = 0;

    for (int i = 0; i < numLeituras; i++)
    {
        digitalWrite(trigPin, LOW);
        delayMicroseconds(2);

        digitalWrite(trigPin, HIGH);
        delayMicroseconds(10);

        digitalWrite(trigPin, LOW);

        long duracao = pulseIn(echoPin, HIGH, 30000);

        // Timeout = 0: sensor sem eco/desconectado. Descarta a leitura
        // em vez de tratar como distancia 0 (que daria 100% falso).
        if (duracao == 0)
        {
            delay(20);
            continue;
        }

        float distancia = duracao * 0.034 / 2;

        float nivel = ((alturaTanque - distancia) / alturaTanque) * 100;

        nivel = constrain(nivel, 0, 100);

        soma += nivel;
        leiturasValidas++;

        delay(20);
    }

    // Nenhuma leitura valida: sinaliza erro de sensor
    if (leiturasValidas == 0)
    {
        return ERRO_SENSOR;
    }

    return soma / leiturasValidas;
}

// ======================================================
// FUNCAO BUZZER
// ======================================================

void beepAlerta(int tempo)
{
    tone(BUZZER, 1500);
    delay(tempo);
    noTone(BUZZER);
}

// ======================================================
// CONTROLE DO RELE (respeita a polaridade do modulo)
// ======================================================

void acionarRele(bool ligar)
{
    if (RELE_ATIVO_BAIXO)
        digitalWrite(RELE, ligar ? LOW : HIGH);
    else
        digitalWrite(RELE, ligar ? HIGH : LOW);
}

// ======================================================
// AUXILIARES DE GRAVACAO NO SD
// ======================================================

void imprimeDoisDigitos(File &arquivo, int valor)
{
    if (valor < 10) arquivo.print('0');
    arquivo.print(valor);
}

void imprimeNivelSD(File &arquivo, float nivel)
{
    if (nivel < 0)
    {
        arquivo.print("ERRO");
    }
    else
    {
        arquivo.print(nivel, 1);
        arquivo.print('%');
    }
}

// ======================================================
// SALVAR HISTORICO NO SD
// ======================================================

void salvarSD(float comb, float agua, float oleo)
{
    File arquivo = SD.open(ARQUIVO_LOG, FILE_WRITE);

    if (arquivo)
    {
        DateTime now = rtc.now();

        // Data e hora com zero a esquerda (ex.: 05/03/2026 09:05:03)
        imprimeDoisDigitos(arquivo, now.day());
        arquivo.print('/');
        imprimeDoisDigitos(arquivo, now.month());
        arquivo.print('/');
        arquivo.print(now.year());

        arquivo.print(' ');

        imprimeDoisDigitos(arquivo, now.hour());
        arquivo.print(':');
        imprimeDoisDigitos(arquivo, now.minute());
        arquivo.print(':');
        imprimeDoisDigitos(arquivo, now.second());

        arquivo.print(" | Comb: ");
        imprimeNivelSD(arquivo, comb);

        arquivo.print(" | Agua: ");
        imprimeNivelSD(arquivo, agua);

        arquivo.print(" | Oleo: ");
        imprimeNivelSD(arquivo, oleo);

        arquivo.println();

        arquivo.close();

        Serial.println("Registro salvo no SD");
    }
    else
    {
        Serial.println("Erro ao abrir SD");
    }
}

// ======================================================
// AUXILIAR DE IMPRESSAO NO SERIAL
// ======================================================

void imprimeNivelSerial(const char *nome, float nivel)
{
    Serial.print(nome);
    Serial.print(": ");

    if (nivel < 0)
        Serial.println("ERRO (sensor)");
    else
    {
        Serial.print(nivel);
        Serial.println("%");
    }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(9600);

    // ==================================================
    // LCD
    // ==================================================

    lcd.init();
    lcd.backlight();

    lcd.setCursor(0, 0);
    lcd.print("Inicializando");

    // ==================================================
    // RTC
    // ==================================================

    if (!rtc.begin())
    {
        lcd.clear();
        lcd.print("Erro RTC");
        while (1);
    }

    // Se o RTC perdeu a hora (bateria fraca / primeira vez),
    // ajusta automaticamente para a data/hora da compilacao.
    if (!rtc.isrunning())
    {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("RTC ajustado para a hora de compilacao");
    }

    // ==================================================
    // SD CARD
    // ==================================================

    if (!SD.begin(chipSelect))
    {
        lcd.clear();
        lcd.print("Erro SD");
        Serial.println("Falha SD");
    }
    else
    {
        Serial.println("SD OK");
    }

    // ==================================================
    // PINOS SENSORES
    // ==================================================

    pinMode(TRIG_COMB, OUTPUT);
    pinMode(ECHO_COMB, INPUT);

    pinMode(TRIG_AGUA, OUTPUT);
    pinMode(ECHO_AGUA, INPUT);

    pinMode(TRIG_OLEO, OUTPUT);
    pinMode(ECHO_OLEO, INPUT);

    // ==================================================
    // LEDs
    // ==================================================

    pinMode(LED_VERDE, OUTPUT);
    pinMode(LED_AMARELO, OUTPUT);
    pinMode(LED_VERMELHO, OUTPUT);

    pinMode(LED_AGUA, OUTPUT);
    pinMode(LED_OLEO, OUTPUT);

    // ==================================================
    // BUZZER E RELE
    // ==================================================

    pinMode(BUZZER, OUTPUT);
    pinMode(RELE, OUTPUT);

    acionarRele(false);   // garante rele desligado na inicializacao

    // ==================================================

    lcd.clear();
    lcd.print("Sistema OK");

    delay(2000);
}

// ======================================================
// LOOP PRINCIPAL
// ======================================================

void loop()
{
    // ==================================================
    // LEITURA DOS NIVEIS
    // ==================================================

    float nivelComb = medirNivel(TRIG_COMB, ECHO_COMB, alturaTanqueComb);
    float nivelAgua = medirNivel(TRIG_AGUA, ECHO_AGUA, alturaTanqueAgua);
    float nivelOleo = medirNivel(TRIG_OLEO, ECHO_OLEO, alturaTanqueOleo);

    // ==================================================
    // SERIAL MONITOR
    // ==================================================

    imprimeNivelSerial("Combustivel", nivelComb);
    imprimeNivelSerial("Agua", nivelAgua);
    imprimeNivelSerial("Oleo", nivelOleo);
    Serial.println("--------------------");

    // ==================================================
    // AVALIACAO DOS ESTADOS
    // ==================================================

    bool combErro = (nivelComb < 0);
    bool aguaErro = (nivelAgua < 0);
    bool oleoErro = (nivelOleo < 0);

    bool combCritico = (!combErro && nivelComb <= COMB_CRITICO);
    bool combAtencao = (!combErro && nivelComb > COMB_CRITICO && nivelComb <= COMB_ATENCAO);
    bool combOk      = (!combErro && nivelComb > COMB_ATENCAO);

    bool aguaBaixa = (!aguaErro && nivelAgua < NIVEL_MIN_MANUT);
    bool oleoBaixo = (!oleoErro && nivelOleo < NIVEL_MIN_MANUT);

    // ==================================================
    // LEDs E RELE (sempre refletem o estado real)
    // ==================================================

    digitalWrite(LED_VERDE,    combOk      ? HIGH : LOW);
    digitalWrite(LED_AMARELO,  combAtencao ? HIGH : LOW);
    digitalWrite(LED_VERMELHO, (combCritico || combErro) ? HIGH : LOW);

    // Modo de reducao de potencia: acionado apenas no combustivel critico
    acionarRele(combCritico);

    digitalWrite(LED_AGUA, (aguaBaixa || aguaErro) ? HIGH : LOW);
    digitalWrite(LED_OLEO, (oleoBaixo || oleoErro) ? HIGH : LOW);

    // ==================================================
    // DISPLAY LCD POR PRIORIDADE
    // (uma unica tela e um unico beep por ciclo)
    //
    // Prioridade:
    //   1. Erro de sensor
    //   2. Combustivel critico
    //   3. Manutencao (agua e/ou oleo baixos)
    //   4. Tela normal de niveis
    // ==================================================

    lcd.clear();

    if (combErro || aguaErro || oleoErro)
    {
        lcd.setCursor(0, 0);
        lcd.print("ERRO SENSOR");

        lcd.setCursor(0, 1);
        if (combErro) lcd.print("C ");
        if (aguaErro) lcd.print("A ");
        if (oleoErro) lcd.print("O ");

        beepAlerta(300);
    }
    else if (combCritico)
    {
        lcd.setCursor(0, 0);
        lcd.print("COMB CRITICO");

        lcd.setCursor(0, 1);
        lcd.print("REABASTECER");

        beepAlerta(500);
    }
    else if (aguaBaixa || oleoBaixo)
    {
        lcd.setCursor(0, 0);
        lcd.print("NIVEL BAIXO:");

        lcd.setCursor(0, 1);
        if (aguaBaixa) lcd.print("AGUA ");
        if (oleoBaixo) lcd.print("OLEO ");
        lcd.print("MANUT");

        beepAlerta(300);
    }
    else
    {
        // Tela normal de niveis
        lcd.setCursor(0, 0);
        lcd.print("C:");
        lcd.print(nivelComb, 0);
        lcd.print("% A:");
        lcd.print(nivelAgua, 0);
        lcd.print("%");

        lcd.setCursor(0, 1);
        lcd.print("O:");
        lcd.print(nivelOleo, 0);
        lcd.print("%");

        if (combAtencao)
        {
            lcd.setCursor(9, 1);
            lcd.print("ATENCAO");
        }
    }

    // ==================================================
    // REGISTRO NO SD
    // ==================================================

    if (millis() - ultimoRegistro > intervaloSD)
    {
        salvarSD(nivelComb, nivelAgua, nivelOleo);
        ultimoRegistro = millis();
    }

    // ==================================================

    delay(1000);
}
