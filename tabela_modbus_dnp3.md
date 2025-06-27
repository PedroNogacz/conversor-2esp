# Tabela de Comandos - Modbus RTU vs DNP3 (1 dispositivo)

| Comando                        | Modbus RTU (Request / Response)                               | DNP3 (Request / Response - Estrutura)                            |
|-------------------------------|----------------------------------------------------------------|------------------------------------------------------------------|
| Ler Coil (0x01)               | Req: `01 01 00 04 00 01 CRC`<br>Resp: `01 01 01 01 CRC`         | Req: FC=READ Grp=1 Var=1 Idx=0004 Qual=17<br>Resp: Val=0x01 Flags=0x01 |
| Ler Discrete Input (0x02)     | Req: `01 02 00 00 00 01 CRC`<br>Resp: `01 02 01 XX CRC`         | Req: Grp=2 Var=1 Idx=0000 Qual=17<br>Resp: Val=0x01 Flags=0x01   |
| Ler Holding Register (0x03)   | Req: `01 03 00 00 00 02 CRC`<br>Resp: `01 03 04 00 06 00 05 CRC`| Req: Grp=30 Var=1 Idx=0000 Qual=17<br>Resp: 2 valores + flags    |
| Ler Input Register (0x04)     | Req: `01 04 00 00 00 02 CRC`<br>Resp: `01 04 04 00 06 00 05 CRC`| Req: Grp=30 Var=2 Idx=0000 Qual=17<br>Resp: 2 valores + flags    |
| Escrever Coil (0x05)          | Req: `01 05 00 0A FF 00 CRC`<br>Resp: Igual ao request         | Req: Grp=12 Var=2 Idx=000A<br>Resp: Ack + flags                  |
| Escrever Registro (0x06)      | Req: `01 06 00 02 00 0C CRC`<br>Resp: Igual ao request         | Req: Grp=40 Var=2 Idx=0002<br>Resp: Ack + flags                  |
| Escrever Múltiplos Coils (0x0F)| Req: `01 0F 00 13 00 0A XX CRC`<br>Resp: `01 0F 00 13 00 0A CRC`| Req: Grp=12 Var=2 Range+BitMap<br>Resp: Ack + flags              |
| Escrever Múltiplos Regs (0x10)| Req: `01 10 00 01 00 02 04 00 0A 01 02 CRC`<br>Resp: `01 10 00 01 00 02 CRC` | Req: Grp=40 Var=2 ValueBlock<br>Resp: Ack + flags        |

Todas as conversões retornam um simples **ACK** confirmando o recebimento do comando.
