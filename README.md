# CPU Switch Control

Controlador de CPU para Linux escrito em **C17**. A interface usa GTK4 e o daemon de frequência usa apenas libc/Linux.

## Recursos

- Detecta automaticamente as políticas `cpufreq` e a faixa comum do processador.
- Exibe uso e frequência atuais.
- Alterna entre clock de baixa e alta carga com histerese configurável.
- Mantém compatibilidade com `/etc/cpu-clock-switch.json` usado pelas versões anteriores.
- Painel de undervolt Intel com cinco domínios: CPU, GPU, CPU Cache, System Agent e Analog I/O.
- Usa `intel-undervolt` como backend quando ele está instalado e o firmware permite o ajuste.
- Não permite overvolt pela interface: os offsets aceitos ficam entre `-150` e `0 mV`.
- Pode habilitar `intel-undervolt.service` para reaplicar a configuração no boot.
- Operações privilegiadas da interface passam por `pkexec`.

## Dependências

### Arch Linux / CachyOS

```sh
sudo pacman -S --needed base-devel gtk4 polkit
```

Para o painel de tensão Intel:

```sh
sudo pacman -S --needed intel-undervolt
```

O painel de frequência funciona sem `intel-undervolt`.

## Compilar

```sh
make
make test
```

## Instalar

```sh
sudo make install
sudo make enable
```

Depois execute:

```sh
cpu-switch-control
```

O serviço instalado é `cpu-clock-switch.service` e executa `/usr/local/bin/cpu-clock-switch-daemon`.

## Configuração

A frequência continua configurada em:

```text
/etc/cpu-clock-switch.json
```

O undervolt Intel usa o arquivo padrão do backend:

```text
/etc/intel-undervolt.conf
```

Ao editar tensão, o aplicativo preserva as demais linhas já existentes em `intel-undervolt.conf` e substitui apenas os cinco registros `undervolt`.

## Como funciona a histerese

Se o uso total cair abaixo do limite inferior, o daemon fixa o clock selecionado para baixa carga. Se subir acima do limite superior, fixa o clock selecionado para alta carga. Entre os dois limites, mantém o estado anterior para evitar oscilações.

## Migração da versão Rust/Python

A versão atual não usa Cargo, Rust nem Python. Os binários são compilados diretamente de:

- `src/main.c` — interface GTK4 e controle de tensão.
- `src/daemon.c` — daemon de frequência.
- `src/config.c` — parser/validador compartilhado da configuração.

O JSON antigo é reutilizado automaticamente.
