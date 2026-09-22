module W = struct
  module F (X : Set.OrderedType) = struct type t = Set.Make(X).t end
  type z = Zed
end
type x = Xed
