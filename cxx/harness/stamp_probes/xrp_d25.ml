module P = struct
  module type A = sig type t = private < m : int; .. > end
  module F (X : A) = struct let y = 1 end
end
