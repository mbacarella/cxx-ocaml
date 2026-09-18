module type E = sig end
let x = Int.zero let _ = (module Int : E)
