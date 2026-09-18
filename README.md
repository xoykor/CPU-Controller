# CPU Switch Control

Controlador de CPU para Linux escrito em **C17**, com interface GTK4 e daemon nativo em C.

## Recursos

- Detecta automaticamente as políticas `cpufreq` e a faixa comum de frequência.
- Exibe uso e frequência atuais.
- Alterna entre clock de baixa e alta carga com histerese configurável.
- Mantém compatibilidade com `/etc/cpu-clock-switch.json` das versões anteriores.
- Painel de undervolt Intel com CPU, GPU, CPU Cache, System Agent e Analog I/O.
- Usa `intel-undervolt` como backend quando ele está instalado e o firmware permite.
- A interface nunca permite offset positivo; a faixa exposta é de `-150` a `0 mV`.
- Pode habilitar `intel-undervolt.service` para reaplicar a tensão no boot.
- A GUI roda sem root e usa `pkexec` somente nas operações que realmente precisam de privilégio.

## Organização do código

O projeto foi dividido para que cada arquivo tenha uma responsabilidade clara:

- `src/main.c`: interface GTK4 e callbacks.
- `src/config.c` / `config.h`: leitura, escrita e validação do JSON.
- `src/cpu_linux.c` / `cpu_linux.h`: acesso a `/proc` e `/sys/.../cpufreq`.
- `src/command.c` / `command.h`: execução de comandos, `pkexec` e systemd.
- `src/undervolt.c` / `undervolt.h`: leitura e atualização de `intel-undervolt.conf`.
- `src/daemon.c`: loop de histerese do serviço de frequência.

Os pontos menos óbvios do código têm comentários explicando a intenção, principalmente ordem de escrita do cpufreq, histerese, preservação da configuração de undervolt e fronteiras de privilégio.

## Dependências

### Arch Linux / CachyOS

```sh
sudo pacman -S --needed base-devel gtk4 polkit
```

Para habilitar o painel de tensão Intel:

```sh
sudo pacman -S --needed intel-undervolt
```

A parte de frequência funciona mesmo sem `intel-undervolt`.

## Compilar e testar

```sh
make test
make
```

O build usa C17 com `-Wall -Wextra -Wpedantic`.

## Instalar

```sh
sudo make install
sudo make enable
```

Depois execute:

```sh
cpu-switch-control
```

## Configuração

A frequência usa:

```text
/etc/cpu-clock-switch.json
```

O undervolt usa o arquivo padrão do backend:

```text
/etc/intel-undervolt.conf
```

Ao salvar tensão, a aplicação preserva comentários e configurações não relacionadas e substitui apenas as cinco linhas `undervolt`.

## Histerese

O daemon trabalha em três regiões:

- uso abaixo do limite inferior: aplica o clock de baixa carga;
- uso acima do limite superior: aplica o clock de alta carga;
- uso entre os dois limites: mantém o estado anterior.

Isso evita ficar alternando rapidamente de frequência quando o uso oscila perto de um limite.
