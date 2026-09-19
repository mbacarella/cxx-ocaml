let _ = let module A = struct module Bad : sig val f : int end = struct let f =
  1 end end in A.Bad.f let z = 1
