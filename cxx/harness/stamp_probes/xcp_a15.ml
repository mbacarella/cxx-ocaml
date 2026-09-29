module W = struct
  module M : sig type t val v : t end = struct type t = int let v = 3 end
  type z = Zed
  module N = struct type q = Q end
  type y = Yed
end
type x = Xed
