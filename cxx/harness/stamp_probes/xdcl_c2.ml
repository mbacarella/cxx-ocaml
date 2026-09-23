module F (P : sig module I : sig val v : int end end) =
  struct let a = P.I.v end
module G (P : sig module I : sig val v : int end end) =
  struct include F (struct module I = P.I end) end
