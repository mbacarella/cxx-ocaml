module G (X : sig type t module M : sig val s : unit type u end end) = struct
  module Y = struct type t module M = struct let s = () type u end end end
