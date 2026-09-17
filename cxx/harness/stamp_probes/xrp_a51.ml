module P = struct
  module type A = sig type t end
  module F (X : A) = struct class c = object method m (x : X.t) = x end end
end
