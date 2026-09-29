module type S = sig type v end
module F (_ : S) = struct type t end
module M = struct type v end
module N = F (M)
