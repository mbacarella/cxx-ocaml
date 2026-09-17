module G (X : sig module M : sig val s : unit val r : unit end end) = struct
  module Y = X end
