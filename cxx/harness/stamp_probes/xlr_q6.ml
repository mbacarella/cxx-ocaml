let _ = let module A = struct module Bad : sig val f : int -> int end = struct
  let f = let y = 5 in fun x -> x+y end end in false let z = 1
