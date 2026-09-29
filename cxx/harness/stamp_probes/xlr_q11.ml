let _ = let module A = struct module Bad : sig val f : int -> int end = struct
  let f x = x end end in false let z = 1
