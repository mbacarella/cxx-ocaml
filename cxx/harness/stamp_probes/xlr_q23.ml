let _ = let module A = struct module Bad : sig val f : int end = struct let f =
  1 end module Bad2 : sig val f : int end = struct let f = 1 end end in false
  let z = 1
