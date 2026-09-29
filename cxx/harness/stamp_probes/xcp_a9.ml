module W = struct
  module V = struct
    module F (X : sig type t end) = struct type u = X.t end
    type z = Zed
  end
  type y = Yed
end
type x = Xed
