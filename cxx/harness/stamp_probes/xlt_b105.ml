module G (X : sig module M : sig val s : unit end end) = struct include X end
