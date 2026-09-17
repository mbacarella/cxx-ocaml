module G (X : sig type t = int val x : t end) = struct
  module Y = X
end
