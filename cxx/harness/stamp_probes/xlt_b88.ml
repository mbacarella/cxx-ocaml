module G (X : sig type t module M : sig val s : unit end end) = struct module
  Y = X end
