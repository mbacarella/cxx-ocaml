module Simple = struct
type 'a t
module type S = sig module Data: sig type t end val key: Data.t t end
module Register (D:S) = struct let key = D.key end
module M = struct module Data = struct type t = int end
  let key : _ t = Obj.magic () end
end
module EM = Simple.Register(Simple.M)
