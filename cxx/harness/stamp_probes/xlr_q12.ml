let _ = let module Bad : sig val f : int -> int end = struct let f x = x end in
  false let z = 1
