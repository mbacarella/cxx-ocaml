module P = struct
  module Q = struct
  module type MyT = sig type t end
  module MyMap(X : MyT) = X
  end
end
