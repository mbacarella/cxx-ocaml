module type S = sig val s : unit end
module G (X : sig type t = int val x : t module M : S end) = struct
  module Y = X
end
