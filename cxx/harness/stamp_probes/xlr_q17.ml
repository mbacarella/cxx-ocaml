let _ = let module A = struct module Bad : sig val f : int val g : int end =
  struct let f = 1 let g = 2 end end in false let z = 1
