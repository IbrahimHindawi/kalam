use std::marker::PhantomData;

pub struct Mesh;
#[cfg(not(test))]
pub struct Texture;
#[cfg(not(test))]
pub struct SkinPalette;
pub struct Handle<T> {
    slot: usize,
    generation: u64,
    kind: PhantomData<T>,
}
impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for Handle<T> {}
pub struct Table<T> {
    slots: Vec<(u64, Option<u32>)>,
    free: Vec<usize>,
    kind: PhantomData<T>,
}
impl<T> Default for Table<T> {
    fn default() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
            kind: PhantomData,
        }
    }
}
impl<T> Table<T> {
    pub fn insert(&mut self, native: u32) -> Result<Handle<T>, String> {
        if native == 0 {
            return Err("GPU asset creation failed".into());
        }
        let slot = match self.free.pop() {
            Some(i) => {
                self.slots[i].1 = Some(native);
                i
            }
            None => {
                self.slots.push((1, Some(native)));
                self.slots.len() - 1
            }
        };
        Ok(Handle {
            slot,
            generation: self.slots[slot].0,
            kind: PhantomData,
        })
    }
    pub fn get(&self, h: Handle<T>) -> Option<u32> {
        self.slots
            .get(h.slot)
            .and_then(|&(g, v)| if g == h.generation { v } else { None })
    }
    #[cfg(not(test))]
    pub fn remove(&mut self, h: Handle<T>, release: impl FnOnce(u32)) {
        let Some(native) = self.get(h) else { return };
        let (generation, slot) = &mut self.slots[h.slot];
        *slot = None;
        if let Some(next) = generation.checked_add(1) {
            *generation = next;
            self.free.push(h.slot);
        }
        release(native);
    }

    pub fn clear(&mut self, mut release: impl FnMut(u32)) {
        for (i, (g, v)) in self.slots.iter_mut().enumerate() {
            if let Some(native) = v.take() {
                release(native);
                if let Some(next) = g.checked_add(1) {
                    *g = next;
                    self.free.push(i);
                }
            }
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn repeated_unload_invalidates_old_handles() {
        let mut table: Table<Mesh> = Table::default();
        let mut count = 0;
        for i in 1..100 {
            let h = table.insert(i).unwrap();
            assert_eq!(table.get(h), Some(i));
            table.clear(|_| count += 1);
            let next = table.insert(900).unwrap();
            assert_eq!(table.get(h), None);
            assert_eq!(table.get(next), Some(900));
            table.clear(|_| count += 1);
            table.clear(|_| panic!("double release"));
        }
        assert_eq!(count, 198);
    }
}
