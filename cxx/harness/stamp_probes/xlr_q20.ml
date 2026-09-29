let _ = let module A = struct module Bad : sig val f : int end = struct let f =
  1 end let z = Bad.f end in false let z = 1
