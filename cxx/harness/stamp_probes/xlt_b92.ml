module G (X : sig module M : sig val s : unit end end) = struct module Y = X end
module A = struct module M = struct let s = () end end
module N = G (A)
