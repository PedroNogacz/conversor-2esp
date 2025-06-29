# conversor-2esp

Este reposit\xc3\xb3rio documenta a fiação e a estratégia de conexão para uma ponte Modbus para DNP3 construída com uma placa ESP32 e uma segunda ESP32. As duas placas traduzem nos dois sentidos: quadros Modbus da primeira ESP32 tornam-se quadros DNP3 na segunda placa, enquanto quadros DNP3 que chegam dessa placa são convertidos de volta para Modbus e encaminhados adiante.

## Disposição do hardware

- **Emissor (Arduino Uno + shield Ethernet W5500)**: Gera comandos Modbus. Periodicamente envia comandos via Ethernet para a primeira ESP32. O shield já contém o chip W5500, então nenhum módulo separado ou fiação extra é necessário \u2013 basta encaixar o shield no Uno.
- **Primeira ESP32-WROOM-32 + W5500**: Recebe comandos Modbus do Arduino pela rede local. Repassa-os por uma conexão serial direta para a segunda ESP32 e também pode enviar dados de volta ao emissor.
- **Segunda ESP32-WROOM-32 + W5500**: Comunica-se com a primeira ESP32 por esse link serial. Quadros Modbus recebidos da primeira ESP32 são encaminhados ao PC como DNP3. Quadros DNP3 vindos do PC s\xc3\xa3o redirecionados para que a primeira ESP32 os converta de volta em Modbus antes de envi\xc3\xa1-los tamb\xc3\xa9m ao PC.
- **PC**: Executa ouvintes para ambos os protocolos e recebe qualquer formato que os conversores enviem.
- **Modem TP-Link de 4 portas**: Fornece conectividade Ethernet para todos os n\xc3\xb3s.

### Aloca\xc3\xa7\xc3\xa3o das portas Ethernet

1. **Porta 1 \u2013 Emissor (Arduino Uno)**
2. **Porta 2 \u2013 ESP32 (lado Modbus)**
3. **Porta 3 \u2013 segunda ESP32 (lado DNP3)**
4. **Porta 4 \u2013 PC**

### Passos de fiação

1. **Conecte cada m\xc3\xb3dulo Ethernet W5500 ao modem TP-Link** usando cabos Ethernet padr\xc3\xa3o, correspondendo às portas indicadas acima.
2. **Ligue as placas ESP32 e segunda ESP32** com uma conex\xc3\xa3o UART direta. Conecte TX (GPIO22) da ESP32 Modbus a RX (GPIO21) da segunda ESP32 e conecte TX (GPIO22) da segunda ESP32 de volta a RX (GPIO21) da ESP32. Esse link serial carrega o comando traduzido da ESP32 para a segunda ESP32.
3. **Alimente cada dispositivo ESP** de acordo com seus requisitos (tipicamente 3,3 V regulados). Certifique-se de que os terras sejam comuns se usar UART entre a ESP32 e a segunda ESP32.
4. **Da segunda ESP32, conecte ao PC** via Ethernet atrav\xc3\xa9s do modem TP-Link. Dependendo do sentido do tr\xc3\xa1fego, o PC pode receber qualquer um dos protocolos.
5. **Ligue um bot\xc3\xa3o de modo ao Arduino Uno**. Conecte um lado de um push button ao pino digital 2 e o outro lado ao GND. O sketch usa o resistor de pull-up interno ent\xc3\xa3o o estado padr\xc3\xa3o seleciona o modo Modbus.

Com essa disposi\xc3\xa7\xc3\xa3o, um comando Modbus do emissor percorre a ESP32 Modbus e depois a ESP32 DNP3 at\xc3\xa9 chegar ao PC. Se um comando DNP3 for enviado, ele percorre a ESP32 DNP3, \xc3\xa9 convertido de volta em Modbus na primeira placa e ent\xc3\xa3o tamb\xc3\xa9m \xc3\xa9 encaminhado ao PC.

### Fiação do W5500 para cada placa

Observa\xc3\xa7\xc3\xa3o: o Arduino Uno usa um shield Ethernet W5500 com as linhas SPI j\xc3\xa1 conectadas aos pinos D10\u2013D13. Basta encaixar o shield no Uno \u2013 nenhum jumper \xc3\xa9 necess\xc3\xa1rio. A tabela abaixo lista os pinos apenas para refer\xc3\xancia.

Os m\xc3\xb3dulos Ethernet W5500 expõem seus pinos SPI com r\xc3\xb3tulos como **S1**, **S2**, **SK**, **S0** e **RST**. Cabecalhos t\xc3\xadpicos tamb\xc3\xa9m incluem terminais nesta ordem: **SCLK**, **GND**, **SCS**, **INT**, **MOSI**, **MISO**, **GND**, **3.3V**, **5V** e **RST**. A tabela abaixo mapeia os sinais principais para os pinos usados neste projeto. O mesmo mapeamento se aplica à ESP32 e à segunda ESP32.

| R\xc3\xb3tulo W5500 | Finalidade | Pino Arduino | Pino ESP32 |
|-------------|---------|-------------|-----------|
| S2 | MISO | D12 | GPIO19 |
| S1 | MOSI | D11 | GPIO23 |
| SK | SCK | D13 | GPIO18 |
| S0 | CS | D10 | GPIO5 |
| RST | Reset | RESET | GPIO16 |
| INT | Interrup\xc3\xa7\xc3\xa3o | n\xc3\xa3o usado | GPIO4 |

Alimente cada m\xc3\xb3dulo W5500 com 3,3 V e conecte os terras à ESP32 e à segunda ESP32. O conector Ethernet de cada W5500 vai para o modem TP-Link usando as portas indicadas acima.

Conecte o terminal **RST** para que a ESP32 ou a segunda ESP32 possa reiniciar o W5500 durante a configura\xc3\xa7\xc3\xa3o. Os sketches de exemplo pulsam o GPIO16 de cada placa para baixo por um curto per\xc3\xadodo na inicializa\xc3\xa7\xc3\xa3o antes de coloc\xc3\xa1-lo em n\xc3\xadvel alto.
#### Conexões dedicadas
Cada terminal do W5500 conecta-se a apenas um pino do microcontrolador. Evite ligar o mesmo sinal do W5500 a mais de um GPIO. O link serial tamb\xc3\xa9m \xc3\xa9 um para um: conecte TX de cada ESP32 somente ao RX da outra placa.


### Resumo de componentes por placa

| Dispositivo | Componentes | Conex\xc3\xb5es |
|--------------------|--------------------------|-------------------------------|
| **Arduino Uno Emissor** | Arduino Uno, shield W5500 | Ethernet na porta 1 |
| **ESP32 Modbus** | Placa ESP32, W5500 | Ethernet na porta 2, UART TX/RX para a segunda ESP32 |
| **Segunda ESP32** | Placa ESP32, W5500 | Ethernet na porta 3, UART TX/RX para a ESP32 Modbus |
| **PC** | PC executando DNP3 master | Ethernet na porta 4 |

A ESP32 e a segunda ESP32 comunicam-se por um link serial TTL direto de 3,3 V. Conecte TX (GPIO22) de cada ESP32 ao RX da outra placa (GPIO21). Ambos os lados operam a 115200 baud usando o formato padr\xc3\xa3o 8N1.

### Refer\xc3\xancia dos pinos seriais

Ambas as placas ESP32 usam UART1 para o link do conversor. Isso exp\xc3\xb5e **TX** (GPIO22) e **RX** (GPIO21) em cada placa. Ligue esses terminais entre as placas conforme mostrado abaixo:

| Placa | R\xc3\xb3tulo do pino | GPIO | Conex\xc3\xa3o para |
|------------------|-----------|------|---------------|
| ESP32 Modbus | TX | 22 | RX da segunda ESP32 (GPIO21) |
| ESP32 Modbus | RX | 21 | TX da segunda ESP32 (GPIO22) |
| Segunda ESP32 | TX | 22 | RX da ESP32 Modbus (GPIO21) |
| Segunda ESP32 | RX | 21 | TX da ESP32 Modbus (GPIO22) |

### Visualiza\xc3\xa7\xc3\xa3o da sa\xc3\xadda de depura\xc3\xa7\xc3\xa3o

Todas as mensagens de diagn\xc3\xb3stico de ambos os sketches ESP32 agora s\xc3\xa3o impressas na porta serial USB da placa a 115200 baud. Abra um monitor serial na conex\xc3\xa3o USB da ESP32 para acompanhar a sequ\xc3\xancia de inicializa\xc3\xa7\xc3\xa3o e ver eventuais erros ou mensagens de heartbeat. Se voc\xc3\xaa vir apenas o log de boot da ROM se repetindo, o firmware provavelmente falhou ao inicializar o m\xc3\xb3dulo Ethernet e est\xc3\xa1 reiniciando at\xc3\xa9 conseguir. Verifique a fiação e a alimenta\xc3\xa7\xc3\xa3o do W5500.

### Bot\xc3\xa3o de sele\xc3\xa7\xc3\xa3o de modo

O sketch do Arduino l\xc3\xaa um push button no pino digital 2 para escolher qual protocolo enviar. No estado n\xc3\xa3o pressionado, o Uno transmite quadros Modbus para a ESP32 Modbus. Pressionar o bot\xc3\xa3o alterna para o modo DNP3 e os mesmos quadros s\xc3\xa3o encapsulados em um cabe\xc3\xa7alho DNP3 m\xc3\xadnimo e enviados para a ESP32 DNP3 a cada dez segundos. Ap\xc3\xb3s a inicializa\xc3\xa7\xc3\xa3o, o emissor espera dez segundos antes de transmitir seu primeiro comando para que a rede possa se estabilizar. Apenas dois dos comandos de exemplo s\xc3\xa3o usados por padr\xc3\xa3o (uma leitura de holding register e uma de input register), mas o sketch pode ser editado para escolher qualquer par da lista em `MODBUS_CMDS`.

Ambos os sketches ESP32 agora verificam essas mensagens. Quando qualquer placa recebe um quadro, ela imprime qual comando de exemplo foi reconhecido ou informa que os bytes n\xc3\xa3o correspondem ao formato esperado. Isso ajuda a confirmar que o link do conversor est\xc3\xa1 funcionando e que os quadros Modbus s\xc3\xa3o preservados dentro do inv\xc3\xb3lucro DNP3. Sempre que uma conex\xc3\xa3o \xc3\xa9 aceita via Ethernet, o log agora inclui o endere\xc3\xa7o IP remoto para voc\xc3\xaa ver qual host enviou o comando.

Cada placa registra o nome do comando com base na tabela em `tabela_modbus_dnp3.md`, ent\xc3\xa3o o significado de cada solicita\xc3\xa7\xc3\xa3o \xc3\xa9 mostrado. Os comandos s\xc3\xa3o numerados sequencialmente \u2013 o emissor imprime ``C1`` e cada conversor imprime o mesmo n\xc3\xba mero ao encaminhar. A resposta correspondente \xc3\xa9 rotulada ``R1`` e assim por diante. Somente o tempo de inicializa\xc3\xa7\xc3\xa3o do emissor e de ambos os conversores \xc3\xa9 impresso; mensagens posteriores omitem timestamps.

### Vis\xc3\xa3o geral da tradu\xc3\xa7\xc3\xa3o

A ESP32 Modbus inclui rotinas simples que envolvem quadros Modbus em um cabe\xc3\xa7alho estilo DNP3 antes de envi\xc3\xa1-los para a segunda ESP32. Dados recebidos nesse formato s\xc3\xa3o desembrulhados de volta para Modbus antes de serem encaminhados ao emissor. Esses exemplos s\xc3\xa3o espa\xc3\xa7os reservados \u2013 substitua-os por manipuladores reais de protocolo em um sistema de produ\xc3\xa7\xc3\xa3o.

Vers\xc3\xb5es anteriores trocavam um curto ``ACK`` entre as duas placas ESP32 ap\xc3\xb3s encaminhar um quadro. Esse handshake interno foi removido, portanto apenas o conversor que est\xc3\xa1 lidando com o comando atualmente responde. O emissor Arduino ainda imprime quaisquer confirma\xc3\xa7\xc3\xb5es que receba para ajudar a confirmar que o comando foi conclu\xc3\xaddo.

Cada sketch imprime um timestamp baseado no seu pr\xc3\xb3prio tempo de atividade sempre que uma mensagem \xc3\xa9 registrada. Quando um comando \xc3\xa9 enviado, o emissor imprime os bytes exatos antes da transmiss\xc3\xa3o e relata ``ACK`` quando o conversor confirma o recebimento. Isso facilita comparar logs entre dispositivos sem depender do hor\xc3\xa1rio da rede.

### Lidando com resets do watchdog

Se qualquer ESP32 reiniciar repentinamente com `Reset reason: 5` (mostrado no log de boot como `TG1WDT_SYS_RESET`), o watchdog disparou. Isso geralmente significa que o c\xc3\xb3digo passou tempo demais dentro de uma fun\xc3\xa7\xc3\xa3o bloqueante. Reveja quaisquer loops longos de inicializa\xc3\xa7\xc3\xa3o ou leitura e insira chamadas `yield()` ou `delay()` curtas para que o watchdog possa executar. Adicionar `Serial.println` ao redor dessas se\xc3\xa7\xc3\xb5es ajuda a identificar onde a aplica\xc3\xa7\xc3\xa3o est\xc3\xa1 travando.

### Protegendo contra travamentos de conex\xc3\xa3o

Fun\xc3\xa7\xc3\xb5es auxiliares nos sketches tentam novamente `Ethernet.begin()` e `connect()` do TCP. Se o W5500 falhar na inicializa\xc3\xa7\xc3\xa3o ou o SPI travar, o c\xc3\xb3digo reinicia o m\xc3\xb3dulo e verifica `Ethernet.hardwareStatus()` antes de tentar novamente. Cada tentativa faz uma pausa com `delay()`/`yield()` para que o watchdog continue rodando. Ap\xc3\xb3s v\xc3\xa1rias tentativas sem sucesso, a placa reinicia, ajudando a se recuperar de falhas de fiação ou rede.


### Ouvinte no PC
Execute `python_receiver_pc.py` no PC para capturar o tr\xc3\xa1fego de ambas as ESP32 conversoras. O script escuta na **porta 20000** por quadros DNP3 da ESP32 DNP3 e na **porta 1502** por quadros Modbus da ESP32 Modbus. Uma janela Tkinter exibe cada mensagem em pain\xc3\xa9is separados enquanto o console imprime as mesmas informa\xc3\xa7\xc3\xb5es.

#### Requisitos

O ouvinte depende apenas da biblioteca padr\xc3\xa3o do Python. Ele usa o toolkit Tkinter, que normalmente acompanha o Python no Windows e macOS. Em distribui\xc3\xa7\xc3\xb5es Debian/Ubuntu, instale com:

```
sudo apt install python3-tk
```

N\xc3\xa3o s\xc3\xa3o necess\xc3\xa1rios pacotes adicionais.

### Configura\xc3\xa7\xc3\xa3o de rede para Windows e modem TP-Link

1. Conecte o Arduino, ambas as placas ESP32 e o PC às quatro portas LAN do modem TP-Link usando cabos Ethernet.
2. Ligue o modem e cada dispositivo. Certifique-se de que os LEDs de link no modem indiquem uma conex\xc3\xa3o em cada porta.
3. Acesse a p\xc3\xa1gina de configura\xc3\xa7\xc3\xa3o do modem (geralmente http://192.168.1.1). Se o DHCP estiver habilitado, reserve os seguintes endere\xc3\xa7os ou configure manualmente cada dispositivo:
   - `192.168.1.50` para o Arduino Uno emissor.
   - `192.168.1.60` para a ESP32 Modbus.
   - `192.168.1.70` para a ESP32 DNP3.
   - `192.168.1.80` para o PC Windows.
4. Certifique-se de que o modem permita tr\xc3\xa1fego nas portas TCP **20000** e **1502**. Se houver firewall ativo, crie regras para liberar essas portas para que o ouvinte em Python possa aceitar conex\xc3\xb5es.
5. No Windows, abra as configura\xc3\xa7\xc3\xb5es do adaptador de rede e atribua o IP est\xc3\xa1tico `192.168.1.80` com m\xc3\xa1scara `255.255.255.0` e o modem como gateway padr\xc3\xa3o.
6. Instale o Python 3 no PC caso ainda n\xc3\xa3o esteja presente. Abra um prompt de comando neste reposit\xc3\xb3rio e execute `python python_receiver_pc.py` para iniciar o ouvinte com interface gr\xc3\xa1fica e console. Passos detalhados est\xc3\xa3o em `WINDOWS_PY_SETUP.md`.

Com esses endere\xc3\xa7os configurados, as placas conversoras conseguir\xc3\xa3o alcancar o PC e os scripts em Python exibir\xc3\xa3o o tr\xc3\xa1fego.
