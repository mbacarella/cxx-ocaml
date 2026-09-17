module G (X : sig module M : sig val s : unit end end) = struct module Y = X
  module Z = X end
