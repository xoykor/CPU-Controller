# CPU Auto Switch

Aplicativo desktop em Rust para configurar o serviço `cpu-clock-switch`.

## O que a interface faz

- Detecta automaticamente as políticas `cpufreq` e a faixa comum de frequência do processador.
- Mostra uso e frequência atuais.
- Permite escolher a frequência de baixa carga e de alta carga dentro da faixa detectada.
- Permite escolher os limites de uso que ativam cada modo.
- Permite ajustar o intervalo de leitura.
- Salva a configuração em `/etc/cpu-clock-switch.json` e pode reiniciar o serviço usando `pkexec`.

O serviço usa histerese: o modo baixo é ativado abaixo do limite inferior e o modo alto acima do limite superior. Entre os dois limites, ele mantém o estado atual para evitar oscilações.

## Compilar e instalar

```bash
cargo build --release
sudo ./install.sh
cpu-switch-control
```

O instalador instala o aplicativo em `/usr/local/bin/cpu-switch-control`, o daemon em `/usr/local/bin/cpu-clock-switch.py`, a unidade systemd e uma configuração inicial.

O serviço precisa executar como root para escrever nos controles de frequência em `/sys`. O aplicativo usa `pkexec` somente ao salvar no arquivo do sistema ou reiniciar o serviço.

## Desenvolvimento

```bash
cargo test
cargo run
```

O serviço antigo `cpu-autoscale.service` não deve permanecer ativo junto com este serviço, pois ambos alteram os mesmos controles de frequência.
