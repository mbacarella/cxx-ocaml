let _ = let module A = struct module rec Bad : sig val f : int -> int end =
  struct let f = fun x -> x end end in false let z = 1
