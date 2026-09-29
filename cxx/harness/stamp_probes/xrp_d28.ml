module P = struct
  module type A = sig type t = private [> `A ] end
  module F (X : A) = struct include X let y = 1 end
end
