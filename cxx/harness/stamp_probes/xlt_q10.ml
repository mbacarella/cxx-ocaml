module type S = sig val s : unit end
module G (X : sig module M : S end) = struct
  module Y = X
end
