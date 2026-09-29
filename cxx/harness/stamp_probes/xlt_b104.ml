module G (X : sig type t module M : sig val s : unit type u end end) = struct
  module Y = struct include X end end
