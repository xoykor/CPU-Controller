use eframe::egui;
use serde::{Deserialize, Serialize};
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

const CONFIG_PATH: &str = "/etc/cpu-clock-switch.json";
const SERVICE_NAME: &str = "cpu-clock-switch.service";
const CPUFREQ_PATH: &str = "/sys/devices/system/cpu/cpufreq";

#[derive(Clone, Debug)]
struct CpuPolicy {
    name: String,
    path: PathBuf,
    min_khz: u64,
    max_khz: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
struct Config {
    low_threshold_pct: f32,
    high_threshold_pct: f32,
    low_frequency_khz: u64,
    high_frequency_khz: u64,
    interval_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            low_threshold_pct: 6.0,
            high_threshold_pct: 10.0,
            low_frequency_khz: 1_200_000,
            high_frequency_khz: 3_500_000,
            interval_ms: 500,
        }
    }
}

impl Config {
    fn for_range(range: (u64, u64)) -> Self {
        Self {
            low_frequency_khz: range.0,
            high_frequency_khz: range.1,
            ..Self::default()
        }
    }

    fn normalize_to_range(&mut self, range: (u64, u64)) {
        self.low_threshold_pct = self.low_threshold_pct.clamp(0.0, 99.0);
        self.high_threshold_pct = self.high_threshold_pct.clamp(1.0, 100.0);
        self.interval_ms = self.interval_ms.clamp(100, 5_000);
        self.low_frequency_khz = self.low_frequency_khz.clamp(range.0, range.1);
        self.high_frequency_khz = self.high_frequency_khz.clamp(range.0, range.1);

        if self.low_frequency_khz > self.high_frequency_khz {
            self.low_frequency_khz = range.0;
            self.high_frequency_khz = range.1;
        }
        if self.low_threshold_pct >= self.high_threshold_pct {
            self.low_threshold_pct = 6.0;
            self.high_threshold_pct = 10.0;
        }
    }

    fn validate(&self, range: Option<(u64, u64)>) -> Result<(), String> {
        if !(0.0..100.0).contains(&self.low_threshold_pct) {
            return Err("O limite inferior de uso deve estar entre 0 e 100%.".into());
        }
        if !(0.0..=100.0).contains(&self.high_threshold_pct) {
            return Err("O limite superior de uso deve estar entre 0 e 100%.".into());
        }
        if self.low_threshold_pct >= self.high_threshold_pct {
            return Err("O limite inferior precisa ser menor que o superior.".into());
        }
        if self.low_frequency_khz > self.high_frequency_khz {
            return Err("A frequência baixa precisa ser menor ou igual à alta.".into());
        }
        if self.interval_ms < 100 {
            return Err("O intervalo mínimo é de 100 ms.".into());
        }
        if let Some((min_khz, max_khz)) = range {
            if self.low_frequency_khz < min_khz
                || self.low_frequency_khz > max_khz
                || self.high_frequency_khz < min_khz
                || self.high_frequency_khz > max_khz
            {
                return Err("As frequências escolhidas estão fora da faixa detectada.".into());
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
struct CpuTimes {
    idle: u64,
    total: u64,
}

struct UsageTracker {
    previous: Option<CpuTimes>,
}

impl UsageTracker {
    fn new() -> Self {
        Self { previous: None }
    }

    fn sample(&mut self) -> Result<f32, String> {
        let current = read_cpu_times()?;
        let usage = self.previous.map(|previous| {
            let total_delta = current.total.saturating_sub(previous.total);
            let idle_delta = current.idle.saturating_sub(previous.idle);
            if total_delta == 0 {
                0.0
            } else {
                (100.0 * (1.0 - idle_delta as f32 / total_delta as f32)).clamp(0.0, 100.0)
            }
        });
        self.previous = Some(current);
        Ok(usage.unwrap_or(0.0))
    }
}

fn read_cpu_times() -> Result<CpuTimes, String> {
    let contents = fs::read_to_string("/proc/stat")
        .map_err(|error| format!("Não foi possível ler /proc/stat: {error}"))?;
    let line = contents
        .lines()
        .find(|line| line.starts_with("cpu "))
        .ok_or_else(|| "A linha de uso global da CPU não foi encontrada.".to_string())?;
    let values: Vec<u64> = line
        .split_whitespace()
        .skip(1)
        .take(8)
        .map(|value| value.parse::<u64>())
        .collect::<Result<_, _>>()
        .map_err(|error| format!("Valores inválidos em /proc/stat: {error}"))?;
    if values.len() < 5 {
        return Err("/proc/stat não contém campos suficientes.".into());
    }
    Ok(CpuTimes {
        idle: values[3] + values[4],
        total: values.iter().sum(),
    })
}

fn read_khz(path: &Path) -> Result<u64, String> {
    let value = fs::read_to_string(path)
        .map_err(|error| format!("Não foi possível ler {}: {error}", path.display()))?;
    value
        .trim()
        .parse::<u64>()
        .map_err(|error| format!("Frequência inválida em {}: {error}", path.display()))
}

fn detect_policies() -> Result<Vec<CpuPolicy>, String> {
    let entries = fs::read_dir(CPUFREQ_PATH)
        .map_err(|error| format!("Não foi possível acessar {CPUFREQ_PATH}: {error}"))?;
    let mut policies = Vec::new();

    for entry in entries {
        let entry = entry.map_err(|error| format!("Falha ao listar políticas de CPU: {error}"))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.starts_with("policy")
            || !entry.file_type().map(|kind| kind.is_dir()).unwrap_or(false)
        {
            continue;
        }
        let path = entry.path();
        let min_khz = read_khz(&path.join("cpuinfo_min_freq"))?;
        let max_khz = read_khz(&path.join("cpuinfo_max_freq"))?;
        if min_khz <= max_khz {
            policies.push(CpuPolicy {
                name,
                path,
                min_khz,
                max_khz,
            });
        }
    }

    policies.sort_by(|left, right| left.name.cmp(&right.name));
    if policies.is_empty() {
        Err("Nenhuma política cpufreq foi encontrada.".into())
    } else {
        Ok(policies)
    }
}

fn common_frequency_range(policies: &[CpuPolicy]) -> Option<(u64, u64)> {
    let min_khz = policies.iter().map(|policy| policy.min_khz).max()?;
    let max_khz = policies.iter().map(|policy| policy.max_khz).min()?;
    (min_khz <= max_khz).then_some((min_khz, max_khz))
}

fn read_current_frequency(policies: &[CpuPolicy]) -> Option<u64> {
    let policy = policies.first()?;
    ["scaling_cur_freq", "cpuinfo_cur_freq"]
        .iter()
        .find_map(|file| read_khz(&policy.path.join(file)).ok())
}

fn load_config() -> Result<Config, String> {
    match fs::read_to_string(CONFIG_PATH) {
        Ok(contents) => serde_json::from_str(&contents)
            .map_err(|error| format!("Configuração inválida em {CONFIG_PATH}: {error}")),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(Config::default()),
        Err(error) => Err(format!("Não foi possível ler {CONFIG_PATH}: {error}")),
    }
}

fn write_system_config(config: &Config) -> Result<(), String> {
    let contents = serde_json::to_string_pretty(config)
        .map_err(|error| format!("Não foi possível serializar a configuração: {error}"))?;
    let temp_path =
        std::env::temp_dir().join(format!("cpu-clock-switch-{}.json", std::process::id()));
    fs::write(&temp_path, format!("{contents}\n"))
        .map_err(|error| format!("Não foi possível preparar a configuração: {error}"))?;

    let result = Command::new("pkexec")
        .arg("install")
        .arg("-m")
        .arg("0644")
        .arg(&temp_path)
        .arg(CONFIG_PATH)
        .output()
        .map_err(|error| format!("Não foi possível iniciar pkexec: {error}"))?;
    let _ = fs::remove_file(&temp_path);
    if !result.status.success() {
        let detail = String::from_utf8_lossy(&result.stderr).trim().to_string();
        return Err(if detail.is_empty() {
            "A autorização administrativa foi cancelada.".into()
        } else {
            format!("Não foi possível salvar a configuração: {detail}")
        });
    }
    Ok(())
}

fn restart_service() -> Result<(), String> {
    let result = Command::new("pkexec")
        .arg("systemctl")
        .arg("restart")
        .arg(SERVICE_NAME)
        .output()
        .map_err(|error| format!("Não foi possível reiniciar o serviço: {error}"))?;
    if result.status.success() {
        Ok(())
    } else {
        let detail = String::from_utf8_lossy(&result.stderr).trim().to_string();
        Err(if detail.is_empty() {
            "O serviço não pôde ser reiniciado.".into()
        } else {
            format!("O serviço não pôde ser reiniciado: {detail}")
        })
    }
}

fn service_state() -> String {
    match Command::new("systemctl")
        .arg("is-active")
        .arg(SERVICE_NAME)
        .output()
    {
        Ok(output) => {
            let state = String::from_utf8_lossy(&output.stdout).trim().to_string();
            if state.is_empty() {
                "indisponível".into()
            } else {
                state
            }
        }
        Err(_) => "indisponível".into(),
    }
}

fn format_frequency(khz: u64) -> String {
    if khz >= 1_000_000 {
        format!("{:.2} GHz", khz as f64 / 1_000_000.0)
    } else {
        format!("{:.0} MHz", khz as f64 / 1_000.0)
    }
}

struct CpuSwitchApp {
    policies: Vec<CpuPolicy>,
    range: Option<(u64, u64)>,
    config: Config,
    usage_pct: f32,
    current_frequency: Option<u64>,
    usage_tracker: UsageTracker,
    service_state: String,
    message: String,
    error: bool,
    next_metrics: Instant,
    next_service_poll: Instant,
}

impl CpuSwitchApp {
    fn new() -> Self {
        let (policies, mut message, error) = match detect_policies() {
            Ok(policies) => (policies, String::new(), false),
            Err(error) => (Vec::new(), error, true),
        };
        let range = common_frequency_range(&policies);
        let mut config = load_config().unwrap_or_else(|error| {
            message = error;
            Config::default()
        });
        if let Some(range) = range {
            config.normalize_to_range(range);
        }
        Self {
            current_frequency: read_current_frequency(&policies),
            policies,
            range,
            config,
            usage_pct: 0.0,
            usage_tracker: UsageTracker::new(),
            service_state: service_state(),
            message,
            error,
            next_metrics: Instant::now(),
            next_service_poll: Instant::now(),
        }
    }

    fn refresh_hardware(&mut self) {
        match detect_policies() {
            Ok(policies) => {
                self.policies = policies;
                self.range = common_frequency_range(&self.policies);
                if let Some(range) = self.range {
                    self.config.normalize_to_range(range);
                }
                self.current_frequency = read_current_frequency(&self.policies);
                self.error = false;
                self.message = "Limites do processador atualizados.".into();
            }
            Err(error) => {
                self.error = true;
                self.message = error;
            }
        }
    }

    fn update_metrics(&mut self) {
        if let Ok(usage) = self.usage_tracker.sample() {
            self.usage_pct = usage;
        }
        self.current_frequency = read_current_frequency(&self.policies);
    }

    fn save(&mut self, restart: bool) {
        if let Err(error) = self.config.validate(self.range) {
            self.message = error;
            self.error = true;
            return;
        }
        if let Err(error) = write_system_config(&self.config) {
            self.message = error;
            self.error = true;
            return;
        }
        if restart {
            if let Err(error) = restart_service() {
                self.message = error;
                self.error = true;
                return;
            }
            self.service_state = service_state();
            self.message = "Configuração aplicada e serviço reiniciado.".into();
        } else {
            self.message = "Configuração salva. Reinicie o serviço para aplicá-la.".into();
        }
        self.error = false;
    }
}

impl eframe::App for CpuSwitchApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let now = Instant::now();
        if now >= self.next_metrics {
            self.update_metrics();
            self.next_metrics = now + Duration::from_millis(500);
        }
        if now >= self.next_service_poll {
            self.service_state = service_state();
            self.next_service_poll = now + Duration::from_secs(2);
        }
        ctx.request_repaint_after(Duration::from_millis(250));

        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("CPU Switch Control");
                let active = self.service_state == "active";
                let color = if active {
                    egui::Color32::GREEN
                } else {
                    egui::Color32::YELLOW
                };
                ui.colored_label(color, format!("Serviço: {}", self.service_state));
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(8.0);
            ui.heading("Controle automático de frequência");
            ui.label("Defina a frequência usada quando o processador estiver ocioso ou sob carga.");

            if !self.message.is_empty() {
                let color = if self.error { egui::Color32::LIGHT_RED } else { egui::Color32::LIGHT_BLUE };
                ui.colored_label(color, &self.message);
            }

            ui.separator();
            ui.horizontal(|ui| {
                ui.label(format!("Uso atual: {:.1}%", self.usage_pct));
                if let Some(frequency) = self.current_frequency {
                    ui.label(format!("Frequência atual: {}", format_frequency(frequency)));
                }
                ui.label(format!("Políticas detectadas: {}", self.policies.len()));
            });

            if let Some((min_khz, max_khz)) = self.range {
                ui.label(format!("Faixa comum detectada: {} — {}", format_frequency(min_khz), format_frequency(max_khz)));
                ui.add_space(8.0);

                ui.group(|ui| {
                    ui.heading("Modo de baixa carga");
                    ui.add(egui::Slider::new(&mut self.config.low_threshold_pct, 0.0..=99.0).step_by(1.0).text("Ativar abaixo de"));
                    ui.label(format!("Abaixo de {:.0}% de uso → {}", self.config.low_threshold_pct, format_frequency(self.config.low_frequency_khz)));
                    ui.add(egui::Slider::new(&mut self.config.low_frequency_khz, min_khz..=max_khz).text("Frequência"));
                    ui.label(format!("Selecionada: {}", format_frequency(self.config.low_frequency_khz)));
                });

                ui.add_space(8.0);
                ui.group(|ui| {
                    ui.heading("Modo de alta carga");
                    ui.add(egui::Slider::new(&mut self.config.high_threshold_pct, 1.0..=100.0).step_by(1.0).text("Ativar acima de"));
                    ui.label(format!("Acima de {:.0}% de uso → {}", self.config.high_threshold_pct, format_frequency(self.config.high_frequency_khz)));
                    ui.add(egui::Slider::new(&mut self.config.high_frequency_khz, min_khz..=max_khz).text("Frequência"));
                    ui.label(format!("Selecionada: {}", format_frequency(self.config.high_frequency_khz)));
                });

                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    ui.label("Intervalo de leitura:");
                    ui.add(egui::Slider::new(&mut self.config.interval_ms, 100..=5_000).step_by(100.0).suffix(" ms"));
                });

                if self.config.low_threshold_pct >= self.config.high_threshold_pct {
                    ui.colored_label(egui::Color32::LIGHT_RED, "O limite inferior deve ser menor que o superior.");
                }
                if self.config.low_frequency_khz > self.config.high_frequency_khz {
                    ui.colored_label(egui::Color32::LIGHT_RED, "A frequência baixa deve ser menor ou igual à alta.");
                }

                ui.add_space(10.0);
                ui.horizontal_wrapped(|ui| {
                    if ui.button("Salvar configuração").clicked() {
                        self.save(false);
                    }
                    if ui.button("Salvar e reiniciar serviço").clicked() {
                        self.save(true);
                    }
                    if ui.button("Restaurar padrões").clicked() {
                        self.config = Config::for_range((min_khz, max_khz));
                        self.message = "Padrões restaurados; clique em salvar para aplicar.".into();
                        self.error = false;
                    }
                    if ui.button("Atualizar detecção").clicked() {
                        self.refresh_hardware();
                    }
                });
            } else {
                ui.colored_label(egui::Color32::LIGHT_RED, "Não foi possível detectar uma faixa cpufreq utilizável.");
                if ui.button("Tentar novamente").clicked() {
                    self.refresh_hardware();
                }
            }

            ui.add_space(12.0);
            ui.separator();
            ui.small(format!("Configuração do serviço: {CONFIG_PATH}"));
            ui.small("Salvar e reiniciar solicita autorização administrativa para atualizar o arquivo e o serviço.");
        });
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([760.0, 670.0])
            .with_min_inner_size([620.0, 560.0]),
        ..Default::default()
    };
    eframe::run_native(
        "CPU Switch Control",
        options,
        Box::new(|_creation_context| Ok(Box::new(CpuSwitchApp::new()))),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn common_range_is_the_intersection_of_policies() {
        let policies = vec![
            CpuPolicy {
                name: "policy0".into(),
                path: PathBuf::new(),
                min_khz: 800_000,
                max_khz: 4_000_000,
            },
            CpuPolicy {
                name: "policy1".into(),
                path: PathBuf::new(),
                min_khz: 1_200_000,
                max_khz: 3_500_000,
            },
        ];
        assert_eq!(
            common_frequency_range(&policies),
            Some((1_200_000, 3_500_000))
        );
    }

    #[test]
    fn invalid_thresholds_are_rejected() {
        let config = Config {
            low_threshold_pct: 20.0,
            high_threshold_pct: 10.0,
            ..Config::default()
        };
        assert!(config.validate(Some((1_000_000, 4_000_000))).is_err());
    }

    #[test]
    fn frequency_is_human_readable() {
        assert_eq!(format_frequency(1_200_000), "1.20 GHz");
        assert_eq!(format_frequency(800_000), "800 MHz");
    }
}
