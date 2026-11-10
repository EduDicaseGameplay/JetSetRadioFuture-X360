# Jet Set Radio Future no Xbox 360

Meu port nativo de Jet Set Radio Future (Xbox original, versão americana) para Xbox 360 com RGH ou JTAG. [Read in English](README.md).

Não é emulador e não é retrocompatibilidade. O código x86 do jogo é traduzido para C++ no PC e compilado para o processador PowerPC da 360. O kernel do Xbox, o Direct3D 8 e o DirectSound foram refeitos sobre as bibliotecas da própria 360. O resultado é um `default.xex` de verdade, gerado a partir da sua cópia do jogo.

**Aqui não tem jogo.** Este repositório não traz arquivos do jogo, nem XEX pronto, nem SDK. Você precisa da sua própria cópia do jogo e de instalar as ferramentas. O código gerado e o XEX contêm o código do jogo: não compartilhe.

## Situação

- 720p, widescreen e 30 fps estáveis por padrão.
- Joguei a garagem e as 14 fases no console, inclusive uma sessão de 20 minutos com 15 trocas de fase.
- A maioria das fases fica em 30 fps, com quedas curtas para perto de 20. As exceções são as multidões de Dogenzaka Hill, que caem até 12 fps, e a 99th Street, que fica em média em 26 fps e cai a 18 no ponto mais cheio. Nesses lugares, a lógica e o desenho do próprio jogo levam mais tempo do que cabe num quadro.
- Os vídeos são pulados: o decodificador WMV original é lento demais na 360.
- O save está ligado (`hdd\E\UDATA`, na pasta do jogo), mas ainda não testei salvar e carregar no console. O mesmo vale para a história, os chefes e o final.

## Controles do port

| Botão | O que faz |
|---|---|
| LB + RB por 1 segundo | Abre o seletor de fases. A vai, B fecha |
| X, no seletor | Modo de vídeo: 720p, ou a resolução original com todos os efeitos |
| Y, no seletor | Ritmo: `30 STEADY` (padrão), `AUTO 60/30` ou `FREE (OLD)` |
| Back + Start por 2 segundos | Volta ao painel |

## O que você precisa

- Xbox 360 com RGH ou JTAG.
- Sua cópia do jogo, versão americana, extraída: `default.xbe` e a pasta `Media`. O SHA-1 do XBE tem de ser `86ffb4081ffb1a8a5da2e03d3838382ce31d7ae8`.
- Windows, 64 bits. Eu compilo no Windows 11.
- **Visual Studio 2010**, exigido pelo SDK. Eu uso a edição Ultimate.
- **Xbox 360 SDK 2.0.21256** (XDK), instalado depois do VS 2010. O instalador cria a variável `XEDK`. Confira com `echo %XEDK%`.
- **Visual Studio 2022 Build Tools**, com a carga "Desenvolvimento para desktop com C++". Ela traz o MSVC, o CMake e o Ninja, usados nas ferramentas do PC.
- **Git**. O CMake usa para baixar o Zydis e o toml++.
- **Python 3**, sem pacotes extras.

## Compilar

1. Copie o seu `default.xbe` para `pc-recomp\assets\default.xbe`.

2. Abra o **x64 Native Tools Command Prompt for VS 2022** e entre na pasta do projeto.

3. Compile as ferramentas do PC, o analisador do XBE e o recompilador:

   ```
   cmake -S x360/tools -B x360/tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build x360/tools/build
   ```

4. Traduza o jogo e compile para a 360:

   ```
   python ice_loop.py
   ```

   Ele cria `pc-recomp\generated` a partir do seu XBE e compila em `x360\build\gen`. Se o compilador do VS 2010 der erro interno em alguma função, o script separa a função e tenta de novo.

5. Compile o runtime:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_runtime.ps1
   ```

6. Faça o link do XEX:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_xex.ps1
   ```

   O resultado fica em `x360\build\default.xex`. No meu PC, o build inteiro, do passo 3 até aqui, levou uns 4 minutos. A maior parte desse tempo é o link.

Se o link parar com `LNK1257`, rode `python x360\tools\ltcg_loop.py`. Ele tira da otimização de link os arquivos em que o linker falha.

## Instalar no console

1. Crie uma pasta no HDD ou num pendrive com `default.xex`, `default.xbe` e `Media` lado a lado. O XBE tem de ficar, porque o port lê dados dele.
2. Deixe uns 300 MB livres. Na primeira vez, o jogo cria um cache em `hdd\Z`, que deixa os carregamentos seguintes mais rápidos.
3. Abra o `default.xex` pelo Aurora, FreeStyle Dash ou XeXMenu.
4. O log fica em `jsrf360.log`, na mesma pasta.

## Arquivos de opção

Arquivos vazios na pasta do jogo:

| Arquivo | Efeito |
|---|---|
| `sem_720p.txt` | 640x480, sem widescreen |
| `sem_widescreen.txt` | 720p em 4:3 |
| `com_hiz.txt` | Liga o Hi-Z, desligado por padrão |
| `sem_pulo.txt` | Sem pulo de quadros: abaixo do fps alvo, o jogo fica em câmera lenta |
| `debug_buttons.txt` | L3 grava uma captura de depuração, Back troca o gama |

`video_modo.txt` e `fps_modo.txt` são gravados pelo seletor de fases para lembrar suas escolhas.

## Créditos

O runtime partiu do meu [port de Bloody Roar Extreme para Xbox 360](https://github.com/EduDicaseGameplay/BloodyRoarExtreme-X360), que usa a ferramenta [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) e o [BloodyRoarExtremeRecomp](https://github.com/bromurok/BloodyRoarExtremeRecomp). Várias correções seguem o port de PC [JSRF Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed), e a lista de funções partiu da [JSRF Decompilation](https://codeberg.org/KeybadeBlox/JSRF-Decompilation). Detalhes e licenças em `THIRD_PARTY.txt` e `pc-recomp`.

Jet Set Radio Future pertence à SEGA. Este é um projeto de fã, sem ligação com a SEGA.
