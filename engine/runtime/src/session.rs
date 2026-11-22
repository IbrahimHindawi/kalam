use crate::{
    lens::Binding,
    scene::Scene,
    walk::{Walker, TICK_HZ},
};
use serde::{Deserialize, Serialize};

const MAX_COMMANDS: usize = 4096;
const MAX_TICKS: u64 = 120_000;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Script {
    version: u32,
    commands: Vec<Command>,
}

#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
enum Command {
    Input { buttons: Vec<Button> },
    Look { dx: f32, dy: f32 },
    Step { ticks: u32 },
    SetSpeed { value: f32 },
    SetExposure { value: f32 },
    Inspect,
}

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum Button {
    Forward,
    Back,
    Left,
    Right,
}

impl Button {
    fn bit(&self) -> u32 {
        match self {
            Self::Forward => 1,
            Self::Back => 2,
            Self::Left => 4,
            Self::Right => 8,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Snapshot {
    pub tick: u64,
    pub eye: [f32; 3],
    pub direction: [f32; 3],
    pub speed: f32,
    pub exposure: f32,
}

#[derive(Debug, Serialize)]
pub struct Report {
    pub version: u32,
    pub tick_hz: u32,
    pub instances: Vec<String>,
    pub observations: Vec<Snapshot>,
    pub final_state: Snapshot,
}

/// Run a bounded command script against a fresh walk simulation. Edits apply
/// to the session's scene copy and are visible in later observations.
pub fn run(mut scene: Scene, script: Script) -> Result<Report, String> {
    if script.version != 1 || script.commands.len() > MAX_COMMANDS {
        return Err(format!(
            "expected script version 1 and at most {MAX_COMMANDS} commands"
        ));
    }
    let config = scene
        .walk
        .as_ref()
        .ok_or("scene has no walk configuration")?;
    let mut walker = Walker::new(config);
    let mut keys = 0;
    let mut observations = Vec::new();
    let snapshot = |walker: &Walker, scene: &Scene| {
        let config = scene.walk.as_ref().unwrap();
        let (eye, target) = walker.view(config);
        Snapshot {
            tick: walker.tick(),
            eye: eye.to_array(),
            direction: (target - eye).to_array(),
            speed: config.speed,
            exposure: scene.exposure,
        }
    };
    for (index, command) in script.commands.into_iter().enumerate() {
        let step = match command {
            Command::Input { buttons } => {
                keys = buttons.iter().fold(0, |mask, button| mask | button.bit());
                Ok(())
            }
            Command::Look { dx, dy }
                if [dx, dy]
                    .into_iter()
                    .all(|value| (-10_000.0..=10_000.0).contains(&value)) =>
            {
                walker.look(dx, dy);
                Ok(())
            }
            Command::Look { .. } => Err("look deltas must be within Â±10,000 pixels"),
            Command::Step { ticks } if walker.tick() + u64::from(ticks) <= MAX_TICKS => {
                walker.step(scene.walk.as_ref().unwrap(), keys, ticks);
                Ok(())
            }
            Command::Step { .. } => Err("script tick budget exceeded"),
            Command::SetSpeed { value } => Binding::WalkSpeed.write(&mut scene, value),
            Command::SetExposure { value } => Binding::Exposure.write(&mut scene, value),
            Command::Inspect => {
                observations.push(snapshot(&walker, &scene));
                Ok(())
            }
        };
        step.map_err(|message| format!("commands[{index}]: {message}"))?;
    }
    let final_state = snapshot(&walker, &scene);
    Ok(Report {
        version: 1,
        tick_hz: TICK_HZ,
        instances: scene.entities.ids().collect(),
        observations,
        final_state,
    })
}
