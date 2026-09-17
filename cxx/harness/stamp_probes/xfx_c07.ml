module type S = sig type v end
module M = struct type v end
module F (X : S) = struct
  type 'a r = { x : 'a }
end
module N = F (M)
let y = { N.x = 1 }
