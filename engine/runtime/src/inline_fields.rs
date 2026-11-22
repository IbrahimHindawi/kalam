use serde::de::DeserializeOwned;
use serde_json::{Map, Value};

/// Resolve inline field metadata before deserializing authored documents.
/// A wrapped field becomes its value, or its default if the value is absent.
pub fn decode<T: DeserializeOwned>(bytes: &[u8]) -> Result<T, String> {
    decode_with(bytes, |_| ()).map(|(value, ())| value)
}

pub(crate) fn decode_with<T: DeserializeOwned, M>(
    bytes: &[u8],
    metadata: impl FnOnce(&Value) -> M,
) -> Result<(T, M), String> {
    let mut value = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    let metadata = metadata(&value);
    let decoded = if resolve(&mut value, "$")? {
        serde_json::from_value(value).map_err(|e| e.to_string())
    } else {
        // Preserve the project's normal deserialization behavior for plain JSON.
        serde_json::from_slice(bytes).map_err(|e| e.to_string())
    }?;
    Ok((decoded, metadata))
}

pub(crate) fn number_range(document: &Value, path: &str, (low, high): (f32, f32)) -> (f32, f32) {
    let Some(field) = document
        .pointer(path)
        .and_then(Value::as_object)
        .filter(|field| is_inline_field(field))
    else {
        return (low, high);
    };
    let bound = |name| field.get(name).and_then(Value::as_f64).map(|n| n as f32);
    (
        bound("min").map_or(low, |min| min.max(low)),
        bound("max").map_or(high, |max| max.min(high)),
    )
}

fn resolve(value: &mut Value, path: &str) -> Result<bool, String> {
    match value {
        Value::Object(fields) if is_inline_field(fields) => {
            *value = resolve_field(fields).map_err(|e| format!("{path}: {e}"))?;
            Ok(true)
        }
        Value::Object(fields) => {
            let mut changed = false;
            for (key, child) in fields {
                changed |= resolve(child, &format!("{path}.{key}"))?;
            }
            Ok(changed)
        }
        Value::Array(items) => {
            let mut changed = false;
            for (index, child) in items.iter_mut().enumerate() {
                changed |= resolve(child, &format!("{path}[{index}]"))?;
            }
            Ok(changed)
        }
        _ => Ok(false),
    }
}

fn is_inline_field(fields: &Map<String, Value>) -> bool {
    fields.contains_key("type")
        && ["value", "default", "min", "max"]
            .iter()
            .any(|key| fields.contains_key(*key))
}

fn resolve_field(fields: &mut Map<String, Value>) -> Result<Value, String> {
    let kind = fields
        .get("type")
        .and_then(Value::as_str)
        .ok_or("inline field type must be a string")?;
    let dimensions = match kind {
        "integer" | "number" => 1,
        "vec2" => 2,
        "vec3" => 3,
        "vec4" => 4,
        "string" | "boolean" => 0,
        _ => return Err(format!("unknown inline field type {kind:?}")),
    };
    if let Some(key) = fields
        .keys()
        .find(|key| !matches!(key.as_str(), "type" | "value" | "default" | "min" | "max"))
    {
        return Err(format!("unknown inline field property {key:?}"));
    }
    let number = |key: &str, value: &Value| -> Result<f64, String> {
        let scalar = if kind == "integer" {
            value
                .as_i64()
                .and_then(|n| i32::try_from(n).ok())
                .map(f64::from)
        } else {
            value.as_f64().filter(|n| n.is_finite())
        };
        scalar.ok_or_else(|| {
            let expected = if kind == "integer" {
                "a 32-bit integer"
            } else {
                "a finite number"
            };
            format!("{key} must be {expected}")
        })
    };
    let bound = |key| fields.get(key).map(|value| number(key, value)).transpose();
    if dimensions == 0 && (fields.contains_key("min") || fields.contains_key("max")) {
        return Err("min and max require a numeric field".into());
    }
    let min = bound("min")?;
    let max = bound("max")?;
    if min.zip(max).is_some_and(|(min, max)| min > max) {
        return Err("min must be less than or equal to max".into());
    }
    let check_number = |key: &str, value: &Value| -> Result<(), String> {
        let n = number(key, value)?;
        if min.is_some_and(|min| n < min) || max.is_some_and(|max| n > max) {
            return Err(format!("{key} is outside the declared range"));
        }
        Ok(())
    };
    for key in ["value", "default"] {
        let Some(value) = fields.get(key) else {
            continue;
        };
        match dimensions {
            0 if kind == "string" && !value.is_string() => {
                return Err(format!("{key} must be a string"))
            }
            0 if kind == "boolean" && !value.is_boolean() => {
                return Err(format!("{key} must be a boolean"))
            }
            0 => {}
            1 => check_number(key, value)?,
            dimensions => {
                let items = value
                    .as_array()
                    .filter(|items| items.len() == dimensions)
                    .ok_or_else(|| format!("{key} must have {dimensions} numeric components"))?;
                for (index, item) in items.iter().enumerate() {
                    check_number(&format!("{key}[{index}]"), item)?;
                }
            }
        }
    }
    fields
        .remove("value")
        .or_else(|| fields.remove("default"))
        .ok_or_else(|| "inline field needs a value or default".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_inline_fields_and_defaults_inside_arrays_and_objects() {
        let raw = br#"{"items":[{"count":{"type":"integer","default":2,"min":0,"max":8}},
            {"count":{"type":"integer","value":5,"default":2,"min":0,"max":8}}],
            "speed":{"type":"number","value":3.5,"min":1},"plain":true}"#;
        let decoded: Value = decode(raw).unwrap();
        assert_eq!(
            decoded,
            serde_json::json!({"items":[{"count":2},{"count":5}], "speed":3.5,"plain":true})
        );
    }

    #[test]
    fn validates_vector_components_and_resolves_text_and_flags() {
        let raw = br#"{"position":{"type":"vec3","default":[1,2,3],"min":0,"max":3},
            "title":{"type":"string","default":"Tuning"},
            "visible":{"type":"boolean","value":false,"default":true}}"#;
        let fields: Value = decode(raw).unwrap();
        assert_eq!(
            fields,
            serde_json::json!({
                "position":[1,2,3], "title":"Tuning", "visible":false
            })
        );
        let raw = br#"{"position":{"type":"vec3","value":[1,2,4],"max":3}}"#;
        assert!(decode::<Value>(raw)
            .unwrap_err()
            .contains("$.position: value[2]"));
    }

    #[test]
    fn invalid_rules_report_the_field_and_the_bad_property() {
        for (rule, expected) in [
            (
                r#"{"type":"integer","value":5,"max":4}"#,
                "value is outside",
            ),
            (
                r#"{"type":"integer","value":5,"default":-1,"min":0}"#,
                "default is outside",
            ),
            (
                r#"{"type":"integer","value":2.5,"min":0}"#,
                "value must be a 32-bit integer",
            ),
            (
                r#"{"type":"number","value":3,"min":4,"max":2}"#,
                "min must be less",
            ),
            (
                r#"{"type":"number","min":0,"max":4}"#,
                "needs a value or default",
            ),
        ] {
            let json = format!(r#"{{"weapon":{{"bounces":{rule}}}}}"#);
            let error = decode::<Value>(json.as_bytes()).unwrap_err();
            assert!(error.starts_with("$.weapon.bounces:"), "{error}");
            assert!(error.contains(expected), "{error}");
        }
    }
}
