module type S = sig type v end
module F (_ : S) = struct type t end
