let _ = let module A = struct module rec Bad : sig val f : int -> int end =
  struct let y = Bad.f 5 let f x = x + y end end in false let z = 1
