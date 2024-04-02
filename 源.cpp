

#include <WiFi.h>  // 包含WiFi库
#include <HTTPClient.h>  // 包含HTTPClient库
#include <ArduinoJson.h>  // 包含ArduinoJson库
#include <SoftwareSerial.h>  // 包含SoftwareSerial库（用于串口通讯）
#include <driver/i2s.h> // 用于配置I2S的驱动
#include "base64.h"
#include "cJSON.h"

// 常量和宏定义
#define key 0
#define ADC 2

// 定义模拟串口的发送端口
#define TX_PIN 17
#define RX_PIN 18


const int DEV_PID = 1537;
const char* CUID = "44950592";
const char* CLIENT_ID = "myQ10rFInKFzFtd6EjRLjMET";
const char* CLIENT_SECRET = "ObU2fYI7xzOCtJTyXKDpxVhV6mHRV4Xw";

const char* ssid = "HONOR";  // WiFi网络名称
const char* password = "wrqcctv123";  // WiFi密码

const char* TTS_URL = "http://tsn.baidu.com/text2audio";  // TTS服务URL
const char* TOKEN_URL = "http://openapi.baidu.com/oauth/2.0/token";  // 获取token的URL

const String ChatMindAiUrl = "https://api.chatanywhere.com.cn/v1/chat/completions";  // ChatMindAi API地址
const String ChatMindAiApiKey = "sk-tetT9sM4MSA8a3LGJcZGxfuyN4c0fAfrcVm9uwqAJA2Yt3bq";  // 替换为你的ChatMindAi API密钥

static const i2s_port_t i2s_num = I2S_NUM_1;  // i2s端口号，注意，如果使用内部DAC，则必须使用I2S_NUM_0

HTTPClient http_client;
hw_timer_t* timer = NULL;
const int recordTimeSeconds = 5;
const int adc_data_len = 16000 * recordTimeSeconds;
const int data_json_len = adc_data_len * 2 * 1.4;
uint16_t* adc_data;
char* data_json;
uint8_t adc_start_flag = 0;
uint8_t adc_complete_flag = 0;
uint32_t num = 0;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t DataIdx = 0;

WiFiClient client;  // 创建WiFi客户端对象

void IRAM_ATTR onTimer();
String gainToken();
void assembleJson(String token);
void sendToSTT();
String getGPTAnswer();
void textToSpeech(const char* text);

String uservoice;
String cleanResult;
String escapedResult;
String prompter = "介绍一下你自己。";
uint32_t time1, time2;

// I2S配置结构体
static const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,    // 采样率16000
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,       // 高优先级中断
    .dma_buf_count = 8,                             // 8个缓冲区
    .dma_buf_len = 1024,                            // 每个缓冲区1K字节，总共8K字节的缓冲区空间
    .use_apll = 0,
    .tx_desc_auto_clear = true,
    .fixed_mclk = -1
};

static const i2s_pin_config_t pin_config = {
    .bck_io_num = 46,                     // 时钟口，对应于MAX38357A的BCLK
    .ws_io_num = 15,                      // 用于声道选择，对应于MAX38357A的LRC
    .data_out_num = 3,                   // ESP32的音频输出口, 对应于MAX38357A的DIN
    //.data_in_num = I2S_PIN_NO_CHANGE      // ESP32的音频输入接口，本例未用到
};


// 函数：将字符串进行URL编码
String urlencode(const String& str) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    for (unsigned int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == ' ') {
            encodedString += '+';  // 将空格替换为加号
        }
        else if (isalnum(c)) {
            encodedString += c;  // 将字母和数字直接添加到编码后的字符串
        }
        else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) {
                code1 = (c & 0xf) - 10 + 'A';  // 获取字符的低4位，并转换为对应的16进制字符
            }
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) {
                code0 = c - 10 + 'A';  // 获取字符的高4位，并转换为对应的16进制字符
            }
            encodedString += '%';  // 添加URL编码的前缀
            encodedString += code0;  // 添加高4位的16进制字符
            encodedString += code1;  // 添加低4位的16进制字符
        }
        yield();  // 放弃CPU控制权，使其他任务有机会执行
    }
    return encodedString;  // 返回编码后的字符串
}



void textToSpeech(const char* text) {
    HTTPClient http;
    String token = gainToken();  // 获取访问令牌
    Serial.println("Access Token: " + token);  // 打印访问令牌
    String encoded_text = urlencode(text);  // 将文本进行URL编码
    // 构建TTS请求参数
    String tts_params = "tok=" + token + "&tex=" + encoded_text + "&cuid=esp32&lan=zh&ctp=1&aue=4&spd=4&pit=5&vol=5&per=0";

    if (http.begin(client, TTS_URL)) {
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");  // 添加HTTP头部
        int httpCode = http.POST(tts_params);  // 发送POST请求

        if (httpCode > 0) {
            if (httpCode == HTTP_CODE_OK) {
                const size_t availableSize = 1 * 1024 * 1024;
                size_t bytesRead = 0;
                char* audioData = (char*)heap_caps_malloc(availableSize, MALLOC_CAP_SPIRAM);
                if (audioData != nullptr) {
                    WiFiClient* stream = http.getStreamPtr();  // 获取HTTP响应流
                    bytesRead = stream->readBytes(audioData, availableSize);  // 读取音频数据
                    // 检查是否读取到了数据
                    if (bytesRead > 0) {
                        // 这里假设DataIdx是在函数外部定义的
                        DataIdx = 0; // 初始化DataIdx                    
                        // 播放音频数据
                        size_t BytesWritten;
                        while (DataIdx < bytesRead) { // 使用bytesRead作为循环条件
                            i2s_write(i2s_num, audioData + DataIdx, 4, &BytesWritten, portMAX_DELAY);
                            DataIdx += 4;

                        }
                        DataIdx = 0; // 重置DataIdx为0

                        Serial.println("语音输出成功");
                    }
                    else {
                        Serial.println("读取音频数据失败");
                    }
                    heap_caps_free(audioData);  // 释放内存
                }
                else {
                    Serial.println("内存分配失败");
                }
            }
            else {
                Serial.print("HTTP错误代码: ");
                Serial.println(httpCode);
            }
            http.end();  // 结束HTTP会话
        }
        else {
            Serial.println("连接失败");
        }
    }
    else {
        Serial.println("无法开始HTTP请求");
    }
}

void sendToSTT() {
    http_client.begin("http://vop.baidu.com/server_api");
    http_client.addHeader("Content-Type", "application/json");
    int httpCode = http_client.POST(data_json);

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http_client.getString();
            Serial.println(payload);
            uservoice = payload;
        }
    }
    else {
        Serial.printf("[HTTP] POST failed, error: %s\n", http_client.errorToString(httpCode).c_str());
    }
    http_client.end();
}

String getGPTAnswer() {
    HTTPClient http;
    http.begin(ChatMindAiUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + ChatMindAiApiKey);
    String data = "\{ \"model\": \"gpt-3.5-turbo\",\"messages\": \[{\"role\": \"user\",\"content\":\"" + prompter + "\"\}\]\}";
    int httpResponseCode = http.POST(data);
    if (httpResponseCode == 200) {
        String response = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, response);
        if (error) {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            return "<error>";
        }
        String answer = doc["choices"][0]["message"]["content"].as<String>();;
        http.end();
        return answer;
    }
    else {
        Serial.print("HTTP POST request failed, error code: ");
        Serial.println(httpResponseCode);
        http.end();
        return "<error>";
    }
}

String gainToken() {
    HTTPClient http;
    String token;
    String url = String("https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=") + CLIENT_ID + "&client_secret=" + CLIENT_SECRET;

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        token = doc["access_token"].as<String>();
        Serial.println(token);
    }
    else {
        Serial.println("Error on HTTP request for token");
    }
    http.end();
    return token;
}
void IRAM_ATTR onTimer()
{
    // Increment the counter and set the time of ISR
    portENTER_CRITICAL_ISR(&timerMux);
    if (adc_start_flag == 1)
    {
        // Serial.println("");
        adc_data[num] = analogRead(ADC);
        num++;
        if (num >= adc_data_len)
        {
            adc_complete_flag = 1;
            adc_start_flag = 0;
            num = 0;
            // Serial.println(Complete_flag);
        }
    }
    portEXIT_CRITICAL_ISR(&timerMux);
}

void assembleJson(String token) {
    memset(data_json, '\0', data_json_len * sizeof(char));
    strcat(data_json, "{");
    strcat(data_json, "\"format\":\"pcm\",");
    strcat(data_json, "\"rate\":16000,");
    strcat(data_json, "\"dev_pid\":1537,");
    strcat(data_json, "\"channel\":1,");
    strcat(data_json, "\"cuid\":\"44950592\",");
    strcat(data_json, "\"token\":\"");
    strcat(data_json, token.c_str());
    strcat(data_json, "\",");
    sprintf(data_json + strlen(data_json), "\"len\":%d,", adc_data_len * 2);
    strcat(data_json, "\"speech\":\"");
    strcat(data_json, base64::encode((uint8_t*)adc_data, adc_data_len * sizeof(uint16_t)).c_str());
    strcat(data_json, "\"");
    strcat(data_json, "}");
}


void setup() {
    SoftwareSerial myserial(RX_PIN, TX_PIN);
    Serial.begin(115200);  // 初始化串口通信
    myserial.begin(9600); // 初始化模拟串口
    pinMode(ADC, ANALOG);
    pinMode(key, INPUT_PULLUP);
    WiFi.disconnect(true);
    WiFi.begin(ssid, password);  // 连接WiFi网络
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        vTaskDelay(200);
    }
    Serial.println("Connected to WiFi");  // WiFi连接成功
    i2s_driver_install(i2s_num, &i2s_config, 0, NULL);        // 安装I2S驱动
    i2s_set_pin(i2s_num, &pin_config);                        // 设置I2S引脚

    timer = timerBegin(0, 40, true);
    timerAlarmWrite(timer, 125, true);
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmEnable(timer);
    timerStop(timer); // 先暂停

    // 动态分配PSRAM
    adc_data = (uint16_t*)ps_malloc(adc_data_len * sizeof(uint16_t));
    if (!adc_data) {
        Serial.println("Failed to allocate memory for adc_data");
    }

    data_json = (char*)ps_malloc(data_json_len * sizeof(char)); // 根据需要调整大小
    if (!data_json) {
        Serial.println("Failed to allocate memory for data_json");
    }

    Serial.println("set设置成功");
}



void loop() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        if (command == "start") {
            Serial.println("开始录音");
            String token = gainToken();
            adc_start_flag = 1;
            timerStart(timer);

            while (!adc_complete_flag) {
                ets_delay_us(10);
            }

            Serial.println("录音结束");
            timerStop(timer);
            adc_complete_flag = 0;

            assembleJson(token);
            sendToSTT();

            String result;
            String prompt;
            DynamicJsonDocument doc(1024);
            deserializeJson(doc, uservoice);
            result = doc["result"].as<String>();
            Serial.println(result);
            prompt = result;
            prompt.trim();
            Serial.println(prompt);

            // 去除方括号
            cleanResult = result.substring(2, result.length() - 2);

            // 打印去除方括号后的结果
            Serial.println("去除方括号后的结果为：" + cleanResult);

            //prompter = Serial.readStringUntil('\n');
            //prompter.trim();
            Serial.println(cleanResult);

            prompter = cleanResult;
            Serial.println(prompter);
            prompter.trim();
            String GPTanswer = getGPTAnswer();
            Serial.println("Answer: " + GPTanswer);
            Serial.println("Enter a prompt:");
            //语音播放

            const char* text = GPTanswer.c_str();
            textToSpeech(text);

            //while (!digitalRead(key));

            Serial.println("Recognition complete");
        }
    }
}

