module G (X : sig module M : sig val s : unit end module L : sig val r : unit
  end end) = struct module Y = X end
