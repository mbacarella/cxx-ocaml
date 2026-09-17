module P = struct
  module type MyT = sig type 'a t = Succ of 'a t end
end
