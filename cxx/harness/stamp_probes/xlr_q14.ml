let _ = let module A = struct module Bad : sig type t end = struct type t = int
  end end in false let z = 1
