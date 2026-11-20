# ZX-Wespi Spectrum — Adaptação do zx-iot-mi para ZX Spectrum

**Base:** [holmatic/zx-iot-mi](https://github.com/holmatic/zx-iot-mi) @ `6881c11` (v0.2.2, 2020-06-26) — fileserver ZX81 completo, sem vídeo. Remote `upstream`.
**Não usar:** o HEAD `dbf0557` do zx-iot-mi ("Refactor - part 1 of many") é um refactor inacabado: traz captura de vídeo/LCD/LED pela metade, desliga o fileserver no `main.c` e não compila (`led_matrix.c`).
**Referência de consulta:** [holmatic/zx-iot-video](https://github.com/holmatic/zx-iot-video) @ `1afd737` (ZX-Wespi-V, remote `upstream-video`). Tem a saída de fita via SPI com resolução de 2 µs (`signal_to_zx.c`), que serve de modelo para a Fase 1. O resto (captura de vídeo, LCD, VGA) está fora do escopo.
**Repositório:** público, `miertink/zx-spec-wespi` (fork de `holmatic/zx-iot-mi`)
**Licença:** GPL-3.0 (herdada do projeto original)
**Branch de trabalho:** `spectrum` — manter `master` intacto como referência do ZX81
**Ambiente:** `. .\tools\idf-env.ps1` e depois `idf.py build`

---

## 1. Objetivo

Adaptar o ZX-Wespi para o ZX Spectrum (48K, posteriormente 128K), criando um fileserver WiFi baseado em ESP32 conectado **exclusivamente pelos conectores EAR/MIC** — totalmente não-invasivo, sem abrir a máquina e sem interface no edge connector.

Fluxo de uso alvo:

1. Usuário liga o Spectrum e digita `LOAD ""`
2. ESP32 injeta no EAR um programa de menu (Z80)
3. Menu lista arquivos do servidor; usuário seleciona
4. Spectrum "salva" a seleção via MIC; ESP32 decodifica
5. ESP32 serve o `.tap` selecionado como sinal de fita no EAR
6. Gestão de arquivos e configuração via webserver do ESP32 (já existente no projeto)

## 2. Diferença arquitetural chave vs. ZX81

| Aspecto | ZX81 (original) | Spectrum (este projeto) |
|---|---|---|
| Leitura de dados do ZX | Sinal amostrado pelo ADC interno (GPIO36) via I2S a 60 kHz (`signal_from_zx.c`) | Captura direta do conector **MIC**: largura de pulso via RMT (ou o ADC atual — avaliar no hardware) |
| Injeção de dados no ZX | Sinal de fita sintetizado no EAR | Igual, mas com codificação de fita do Spectrum |
| Detecção de estado da máquina | Análise do frame de vídeo (SLOW 50/60Hz, SAVE, silêncio) | Nenhuma: o loader toca em loop desde o boot e o `LOAD ""` pega o próximo cabeçalho (ver §4.1) |
| Formato de arquivo | `.p` + charset ZX81 | `.tap` (blocos flag+dados+checksum XOR) |
| Invasividade | Requer tap interno no vídeo | Zero — só plugs de áudio |

## 3. Mapa de módulos

### Situação em 2026-11-20
O código do ZX81 (`signal_to_zx`, `signal_from_zx`, `zx_server`, `zx_serv_dialog`, `zx_file_img`, `asm_code.c`, os programas Z80 `loader`/`menu1k`/`strin1k`, `Makefile`/`component.mk`) foi removido. Do ZX-Wespi restam, reescritos ou adaptados: `wifi_sta.c` (agora com scan sob demanda e troca de rede pelo menu), `file_server.c` (servidor web, upload, OTA), `main.c`, partições e NVS. Mapa atual dos módulos e uso: ver `README.md`.

### Plano original
- Reaproveitar: `wifi_sta.c`, `file_server.c`, `main.c`, `zx_server.c`, partições, OTA, NVS

### Fora do escopo: vídeo / display (decisão de 2026-11-16)
Sem captura de vídeo, VGA ou LCD. Motivo: diferente do ZX81, a linha Spectrum é muito variada em saída de vídeo. O 48K só tem RF; RGB externo existe apenas no 128K/+2 cinza (com BRIGHT) e no +2A/+3 (sem BRIGHT). Capturar o vídeo colorido nos modelos mais simples exigiria abrir e soldar, o que viola o princípio de não-invasividade. O projeto fica restrito a EAR/MIC.

Reescrever (plano original; tudo concluído ou substituído):

| Módulo | Ação |
|---|---|
| `signal_to_zx.c` | Substituído por `tape_out.c` (encoder) + `tap_player.c` (leitura do `.tap`). Saída via **RMT** no GPIO22, resolução de 0,1 µs, sem deriva acumulada. RMT em vez de SPI: o SPI deixa pequenos intervalos entre transferências DMA, com o nível do pino fora de controle, o que pode criar bordas espúrias, e a ROM do Spectrum conta bordas |
| `signal_from_zx.c` | Decodificar pulsos do MIC (largura de pulso) e remover as heurísticas de vídeo do ZX81. Opções: RMT (medição em hardware) ou manter o ADC+I2S atual (16,7 µs de resolução já separa 244 de 489 µs e pode dispensar o comparador do §6 — hipótese a medir) |
| `zx_file_img.c` | Parser/gerador `.tap`; remover charset ZX81 (Spectrum usa ASCII no range imprimível) |
| `main/asm/` | Novo loader/menu Z80 para Spectrum (ver §5) |
| `zx_serv_dialog.c` | Adaptar protocolo de diálogo ao novo menu |

## 4. Especificação do sinal de fita (Spectrum)

Referência de timing em T-states (clock Z80 = 3,5 MHz; 1 T ≈ 0,2857 µs):

| Elemento | T-states | Duração aprox. |
|---|---|---|
| Pulso de piloto | 2168 | 619 µs (metade de ciclo) |
| Piloto — header | 8063 pulsos | ~5 s |
| Piloto — data block | 3223 pulsos | ~2 s |
| Sync 1 | 667 | 190 µs |
| Sync 2 | 735 | 210 µs |
| Bit 0 | 2× 855 | 2× 244 µs |
| Bit 1 | 2× 1710 | 2× 489 µs |

Estrutura `.tap`: `[len 2B LE][flag 1B: 0x00 header / 0xFF data][payload][checksum XOR 1B]`. Header padrão: 17 bytes (tipo, nome 10 chars, comprimento, param1, param2).

Notas de implementação (plano inicial; o que valeu está nas fases abaixo):
- ~~I2S TX gerando a forma de onda por amostras~~ → RMT, 0,1 µs, sem deriva (Fase 1).
- Turbo loader: feito como carga sob demanda + turbo de 2 bits por pulso, ~3× a ROM (Fase 4).

### 4.1 Loader em loop (decisão de 2026-11-17)

Não há como saber pelo MIC que o Spectrum entrou em `LOAD ""`: o LOAD só escuta o EAR, e o único rastro (`LD A,$0F / OUT ($FE),A` em LD-BYTES, bit 3 do MIC = 1) é o mesmo nível que o clique de tecla (BEEPER, `OR $08`) já deixou. Em vez de detectar, o ESP32 toca o loader em loop desde o boot, com piloto encurtado (`ZXS_LOOP_PILOT_PULSES`, padrão 3223 ≈ 2 s). Ao ver uma borda, a ROM espera ~1 s e precisa de 256 ciclos de piloto (~0,32 s); se começar a escutar no meio de um bloco, pega o cabeçalho seguinte (LD-LOOK-H ignora blocos com flag errado). Botão BOOT (GPIO0, `ZXS_BUTTON_GPIO`) liga/desliga o loop; a página web tem Play/Loop/Stop.

Riscos medidos no TK90X: o sinal do EAR vaza para o MIC (confirmado). Resolvido: o menu avisa o ESP32 pelo MIC (`M`) e o loop para; blocos ouvidos enquanto o ESP32 transmite são descartados como eco. Depois de um programa carregado a fita fica muda; o loop só volta num reset do Spectrum, detectado pelo MIC (`ZXS_MENU_ON_RESET`).

## 5. Software Z80 (lado Spectrum)

- **Menu**: programa BASIC+CODE carregado via `LOAD ""`. Exibe lista de arquivos paginada.
- **Seleção → servidor**: `SAVE` de um bloco curto contendo o índice/nome do arquivo. ESP32 decodifica pelo MIC.
- Rotinas ROM úteis: `LD-BYTES` em `0x0556`, `SA-BYTES` em `0x04C2`.
- Toolchain sugerida: `sjasmplus` ou `pasmo` (o original usa TASM; migrar).
- Manter o menu compatível com 48K (limite conservador de RAM); 128K depois.

## 6. Hardware / interface

- **EAR (ESP32 → Spectrum)**: GPIO22 ligado direto ao EAR (níveis de 3,3 V), gerado pelo RMT. Funciona no TK90X sem divisor.
- **MIC (Spectrum → ESP32)**: no TK90X a saída da ULA passa por 10k em série com 100 Ω ao GND: ~50 mV, baixa impedância (nível de microfone de gravador). Fica na zona morta do ADC perto de 0 V, por isso a entrada é acoplada por capacitor e polarizada na faixa linear do ADC (0 dB):
  ```
  MIC --[1k]--||-- 100 nF --+-- GPIO36 (VP)
                            +-- 100k -- 3V3
                            +-- 22k  -- GND     (polarização ~0,6 V; 4k7 original retirado)
  ```
  Alternativa mais robusta, se o ruído atrapalhar: comparador (LM393 com histerese) entregando nível lógico a um GPIO.
- Alimentação: USB no ESP32 (independente do Spectrum).
- Pinos: EAR = GPIO22 (Kconfig `ZXS_TAPE_OUT_GPIO`), MIC = ADC1_CH0 / GPIO36 (`mic_in.c`), botão = GPIO0 (`ZXS_BUTTON_GPIO`), LED = GPIO2 e GPIO21 (`main.c`). Esquema completo no `README.md`.

## 7. Ambiente de build

- **ESP-IDF v4.4.8** em `C:\Espressif` (fora do Dropbox), Python 3.11 dedicado (venv `idf4.4_py3.11_env`). O padrão do sistema é 3.14, que não foi validado com a v4.4. Assembler Z80: sjasmplus 1.24.0 em `C:\Espressif\tools\sjasmplus`. A pasta `build/` está marcada como ignorada pelo Dropbox.
- **ESP-IDF v4.4 LTS** — obrigatório nesta fase. O código usa o driver I2S legado (`driver/i2s.h`) e a API antiga de ADC, incompatíveis com v5.x. Migração para v5 só depois da baseline funcional.
- VS Code + extensão ESP-IDF, target `esp32`.
- Validar primeiro o build do `master` (ZX81) intacto antes de qualquer alteração — separa problemas de ambiente de problemas de código.

## 8. Roadmap

**Fase 0 — Baseline**
- [x] Build do `master` (zx-iot-mi `6881c11`) em ESP-IDF v4.4.8, sem alterações de código (só o `sdkconfig` regravado pelo IDF) — 2026-11-16
- [x] Branch `spectrum` criado
- [x] Flash e teste no hardware: ESP32 sobe, conecta no WiFi, webserver responde — 2026-11-16 (ESP32-D0WD-V3 rev 3.1, COM4)

Credenciais WiFi: o original grava pelo menu do ZX81. Sem ele, preencher `wifi.local.csv` (ignorado pelo Git) e gravar só a partição NVS:
```
python $env:IDF_PATH\components\nvs_flash\nvs_partition_generator\nvs_partition_gen.py generate wifi.local.csv build\wifi.nvs.bin 0x4000
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py --chip esp32 -p COM4 write_flash 0x9000 build\wifi.nvs.bin
```
Observação: no IDF 4.4 o driver I2S legado gera um MCLK no GPIO0 (pino de boot). Não atrapalha em operação; some quando o EAR migrar para SPI (Fase 1).

**Fase 1 — Encoder (ESP32 → Spectrum)**
- [x] Encoder com os timings do §4 (`tape_out.c`, RMT) e clock do Z80 configurável no Kconfig (`ZXS_CPU_CLOCK_HZ`)
- [x] Player de `.tap` com validação de checksum (`tap_player.c`)
- [x] Gatilho provisório: botões Play/Stop no webserver (até o menu da Fase 3)
- [x] Arquivo de teste `taps/hello.tap` (BASIC com autostart), gerado por `tools/make_tap.py`
- [x] Teste no Wespi (sem Spectrum): blocos tocados com a duração prevista (cabeçalho 5,095 s, dados 2,245 s) — 2026-11-16
- [ ] Conferir no osciloscópio/frequencímetro: piloto = onda quadrada de 807,2 Hz (meio ciclo de 619 µs)
- [x] Critério de aceite: TK90X carrega o `hello.tap` via `LOAD ""` e mostra HELLO WORLD — 2026-11-17

Hardware de teste: o ZX-Wespi montado, regravado com o firmware Spectrum. Backup completo da flash ZX81 em `backup/` (fora do Git), conferido com `verify_flash`; para restaurar: `esptool.py -p COM4 write_flash 0 backup\<arquivo>.bin`.

**Fase 1b — Loader em loop (§4.1)**
- [x] Loop desde o boot do `ZXS_LOOP_FILE`, piloto encurtado, botão BOOT e botão Loop na página
- [x] Critério de aceite: TK90X ligado com o loop já tocando; `LOAD ""` a qualquer momento carrega o `hello.tap` — 2026-11-17, várias tentativas em momentos diferentes
- [ ] Medir: zumbido no alto-falante do TK90X durante o loop

**Fase 2 — Decoder (Spectrum → ESP32)**

Achados no TK90X (2026-11-17), entrada original do Wespi (1k em série, 4k7 do VP ao GND):
- ADC conferido com 3V3 na entrada: ~3300 em 12 dB, 4095 em 0 dB.
- O IDF 4.4 ignora `adc1_config_channel_atten()` no modo I2S (`adc_i2s_mode_init()` fixa 12 dB). O `mic_in.c` reescreve a tabela de padrões do ADC para 0 dB (~0–1 V) depois do `i2s_adc_enable()`.
- MIC do TK90X: o osciloscópio indicava ~0,5 V, incompatível com o divisor 10k/100 Ω do esquema (máx. ~50 mV); provável fator ×10 de ponta/canal. O ADC lia ~0: o sinal real estava na zona morta.
- Vários "zeros" iniciais foram mau contato do plug do MIC.

Com a entrada acoplada e polarizada (§6), 2026-11-18, ESP32 trocado:
- Repouso ~2278 contagens; SAVE com ~±300 em torno disso. Ruído de repouso ~±200 em picos de uma amostra (ADC com WiFi ligado), ~200 bordas falsas por janela de 250 ms, sem impedir a decodificação.
- O EAR vaza para o MIC (rede de resistores comum no pino da ULA): o decoder leu o próprio loop do Wespi com checksum OK. O protocolo do menu precisa de flag própria e de descartar blocos lidos enquanto o loop transmite.
- [x] Decoder (`mic_in.c`): média móvel (~70 ms) como referência, borda por desvio ±25 com 2 amostras de confirmação, pilot ≥256 meios-ciclos, sync, bits por soma dos dois meios-ciclos, checksum. (A captura bruta `/micarm` + `/miccap` usada no desenvolvimento foi removida.)
- [x] Critério de aceite: `SAVE "a"` do TK90X decodificado — cabeçalho (programa "a", 44 bytes) e bloco de dados de 46 bytes com checksum OK — 2026-11-18
- [x] Robustez ao ruído — 2026-11-19: filtro de glitches (meios-ciclos < 110 µs reagrupados pela paridade e pela plausibilidade), tolerância a até 6 meios-ciclos espúrios no piloto, limite de comprimento. Validado com um SAVE de 16 KB (ROM inteira) com checksum OK. O rádio do ESP32 é a principal fonte de ruído: o `/status` da página é consultado a cada 2 s e só com a aba visível.

**Fase 3 — Menu e protocolo**

Menu (2026-11-18; versão atual de 2026-11-20): `main/asm/spmenu.asm` (sjasmplus) → `spmenu.bin`, embutido no firmware. O `zx_menu.c` monta um programa BASIC e grava `/spiffs/_menu.tap`, que o player toca em loop desde o boot; refeito quando os arquivos mudam (e com o IP novo):
```
 1 REM <código do menu><geração><n><linha de info><arquivos: tipo, KB, nome>
10 IF USR 23760=1 THEN GO TO 20
15 LOAD ""
20..60  senha do WiFi: INPUT LINE p$, copiada para 0x7780, enviada (USR 23763)
```
Protocolo de resposta pelo MIC: bloco SAVE com flag `0xA5` (≥0x80: pilot curta da ROM), 4 bytes `comando, geração, índice lo, índice hi`.
- `M` — menu aberto: o ESP32 para o loop. Sem escolha em 2 min, o loop volta.
- `L` — carregar o arquivo `índice`: o ESP32 toca o `.tap`; 3 s depois de terminar, o menu volta ao loop (`ZXS_MENU_RELOOP`). O BASIC do menu já executa `LOAD ""`.
- `O` — módulo `índice` (1 WiFi, 2 info): ver Fase 5.
- `W` — rede escolhida no módulo WiFi: id da lista, rede, tamanho da senha, senha.
- `T` / `E` — pedido de bloco / erro de bloco turbo (Fase 4).
- A geração muda quando a lista de arquivos muda: resposta de um menu antigo é recusada.
- O menu só transmite após 200 ms sem bordas no EAR, e o player não inicia bloco enquanto o MIC recebe (`mic_in_is_receiving()`).

- [x] Menu Z80 + protocolo `M`/`L`
- [x] Critério de aceite: ciclo completo no TK90X — `LOAD ""`, menu, seleção, `hello.tap` carregado (HELLO WORLD) — 2026-11-18
- [ ] Experiência equivalente à do ZX81 original (ver análise do `master`)

**Fase 4 — Refinamentos**

Formatos de fita (2026-11-18): o `tape_out` virou um fluxo genérico de pulsos (anel de 2048 entradas `[nível:1][T-states:15]` consumido pela ISR do RMT), e o `tape_file.c` toca, sobre ele:
- `.tap` — como antes (pilot encurtada no loop do menu).
- `.tzx` v1.20 — todos os blocos de sinal (0x10 padrão, 0x11 turbo, 0x12 tom, 0x13 pulsos, 0x14 dados puros, 0x15 gravação direta, 0x18 CSW, 0x19 dados generalizados), pausa, nível, loop, jump, call/return e "stop the tape". Blocos informativos são logados ou ignorados.
- `.pzx` — PULS, DATA, PAUS, STOP.
- `.csw` v1/v2 — RLE e Z-RLE (zlib inflado com o `tinfl` da ROM; precisa de ~44 KB livres).
- "Stop the tape" (jogos multi-load): o player para e guarda a posição; tocar o mesmo arquivo de novo (Play na web) continua dali. O menu não volta ao loop nesse caso.
- Bloco de dados lido para a RAM antes de tocar se sobrar ≥24 KB de heap; senão é lido do arquivo durante a reprodução.
- Validado no PC: todos os formatos geram a mesma sequência de pulsos do `.tap` de referência; ISR testada com simulação do driver RMT do IDF 4.4 (sem underrun, bordas de início/fim corretas). **Falta validar no TK90X.**

Fora do escopo (decisão de 2026-11-18):
- `.trd` / `.scl` — imagens de disco TR-DOS: precisam de interface Beta Disk. Possível no futuro: extrair arquivos BASIC/CODE para `.tap` (só funciona com programas que não chamam o TR-DOS).
- `.z80` / `.sna` — snapshots: seriam viáveis para 48K gerando um loader (dados + código que restaura os registradores), mas ficam de fora.
- `.wav` — não cabe no SPIFFS (~960 KB; um jogo de 48K em WAV tem vários MB).

- [x] TZX/PZX/CSW — TZX validado no TK90X (Profanation carregou inteiro); PZX e CSW só no PC
- [x] Turbo loader — 2026-11-19: carga sob demanda + turbo de 2 bits (SPACING 10, calibração do piloto em 32 pulsos), ~3,1× a ROM, repetição automática de bloco com erro (até 2 vezes). Validado no TK90X com o curso multi-parte (tkdemo, lições 1 e 2)
- Programa sem RAM livre para a rotina turbo (perto do seu CLEAR): rotina mínima só de pedido (26 bytes), LOADs pela ROM com pilot curta.
- "Fita posicionada" (loop do próximo bloco após inatividade) testada e removida em 2026-11-19: o sinal vaza do EAR para o MIC e prendia o decodificador de pedidos. A continuação de fitas encadeadas vem dos próprios LOADs dos programas.
- Programa carregado pelo menu sem linha de autostart: o ESP32 acrescenta LINE (última linha + 1); sem isso o BASIC voltava para a linha 10 do menu ("C Nonsense in BASIC, 10:2").
- Profanation (TZX): carrega inteiro mas não responde ao teclado no TK90X, com qualquer nível de EAR e com o cabo solto. A ROM do Rodolfo Guerra confere com a do 48K nas rotinas de teclado e fita (dump pela gravação de SAVE). Conclusão: incompatibilidade do jogo com o hardware do clone, não do servidor.

**Fase 5 — Página web, menu modular e WiFi pelo Spectrum (2026-11-19/20)**

Página: painel da fita dinâmico (bloco, bytes, velocidade, tempo restante), painel do SAVE em andamento, botão Continuar, três idiomas (pt-BR, es-ES, en-US) pelo navegador ou pelo rodapé, logotipo de cassete com carretéis girando, upload de vários arquivos de uma vez (o menu é refeito uma vez, 2 s depois do último), arquivo do menu protegido contra exclusão e renomeação. A página é enviada em blocos de 1400 bytes; se o navegador sai no meio, o resto é descartado (antes travava o servidor).

Menu modular: o que não é escolher arquivo saiu do programa que carrega a cada partida. O menu pede um módulo (`O`), o ESP32 o monta na hora com dados frescos e o toca como um bloco sem cabeçalho (flag 0xA6, piloto de 1,6 s); o menu o carrega em 0x7800 com o `LD-BYTES` da ROM, confere tamanho e checksum e executa. O menu caiu de 2,5 KB para 1,7 KB (~5 s a menos por carga); a pausa depois do cabeçalho no loop caiu de 1 s para 0,3 s.
```
OVL+0  tamanho do payload (2 = sem módulo: menu desatualizado, o ESP32 toca o menu novo)
OVL+2  entrada pelo menu (A = 0 volta à lista, 1 pede a senha no BASIC)
OVL+5  entrada pelo BASIC (USR 23763: envia a escolha do WiFi)
OVL+8  `list` do menu com que foi montado (o firmware recusa módulo de outro menu)
```
- `spinfo.asm`: endereço web, rede e sinal, MAC, espaço livre, versão, ajuda das teclas.
- `spwifi.asm`: redes ao alcance (scan na hora do pedido; o scan periódico em segundo plano saiu), escolha por letra, senha no `INPUT LINE` do BASIC (editor da ROM: todos os caracteres), envio `W`. O ESP32 grava na NVS e conecta; a senha nunca vai para o log.
- O menu espera no máximo 15 s pela primeira borda do módulo; sem resposta, avisa e volta à lista (o `LD-BYTES` da ROM esperaria para sempre).
- `main/asm/build.cmd` monta o menu e depois os módulos (que incluem os rótulos exportados em `spmenu.exp`).

Idioma único (2026-11-20): a escolha (bandeiras pequenas no rodapé da página, no lugar do PT·ES·EN; teclas 1-3 na tela de info do Spectrum, resposta `G`) fica na NVS (`zxstorage`/`lang`) e vale para a página e para o menu. Sem escolha, a página segue o navegador e o menu fica em inglês. Os textos do menu e dos módulos saíram do código Z80: o ESP32 os manda com os dados (`zx_text.c`, sem acentos, larguras conferidas por script na geração), com a bandeira do idioma (4 caracteres de atributo + 8 linhas de pixels, `draw_flag`) no canto superior direito. O menu ficou com ~1,9 KB (+0,15 KB pela bandeira e textos).

- [x] Página web, i18n, upload múltiplo, painel do SAVE — validados
- [x] Idioma pela página: menu refeito no idioma escolhido (conferido decodificando o `_menu.tap` nos três idiomas)
- [ ] Idioma pelo Spectrum (SPACE, 1-3) e bandeiras na tela — teste no TK90X pendente
- [x] WiFi pelo menu (versão sem módulos) — gravado; teste da troca de rede no TK90X pendente
- [ ] Critério de aceite dos módulos no TK90X: SPACE mostra a info, W lista as redes e troca a rede, `0` volta, pedido perdido avisa

**Instalação para leigos (2026-11-20, v1.0.1)**

Página `docs/index.html` (GitHub Pages: `miertink.github.io/zx-spec-wespi`) em PT/ES/EN com o ESP Web Tools 10.4.0: Chrome/Edge, USB, botão Instalar. `tools/make_release.py` junta bootloader, partições, otadata e app num `.bin` único gravado em 0x0 (também anexado às releases, para o Flash Download Tool). Instalar apaga a NVS (rede, idioma), as fitas ficam. A rede padrão passou a ser vazia: sem rede, o ESP32 espera a escolha pela tecla W, sem tentar conectar a `myssid`. Testado: imagem única gravada em 0x0 numa NVS vazia, boot limpo, 10 fitas preservadas.

- [ ] Testar no TK90X a escolha de rede (W) a partir de um ESP sem rede nenhuma

**Pendências e roadmap**
- [ ] Modo de carregamento por arquivo/micro: normal (ROM, sinal original), rápido (ROM, pilot curta) e turbo; escolha no menu do Spectrum (novo módulo) e padrão na página web, para micros onde o turbo não funcione bem (decisão de 2026-11-19)
- [ ] Suporte 128K
- [ ] Migração ESP-IDF v5.x
- [ ] Conferir o piloto no osciloscópio; medir o zumbido do loop no alto-falante do TK90X

## 9. Referências

- ZX Spectrum ROM disassembly (rotinas de fita): "The Complete Spectrum ROM Disassembly", Logan & O'Hara
- Formato TAP/TZX: especificação em worldofspectrum.org
- Projeto original: holmatic/zx-iot-mi (base) e holmatic/zx-iot-video (consulta: saída de fita via SPI)
- Hardware de referência de nível EAR/MIC: ZX Spectrum Service Manual
