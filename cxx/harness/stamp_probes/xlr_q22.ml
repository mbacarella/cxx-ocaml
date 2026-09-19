let _ = let module A = struct module B = struct module Bad : sig val f : int end
  = struct let f = 1 end end end in false let z = 1
