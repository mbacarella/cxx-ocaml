module G (X : sig module M : sig val s : unit end end) = struct module Y =
  struct module M = X.M end end
