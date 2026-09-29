module G (X : sig module M : sig module N : sig val s : unit end end end) =
  struct module Y = X end
