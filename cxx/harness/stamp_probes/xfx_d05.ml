module type S = sig type v end
module F (X : sig type w end) (Y : sig type w2 end) = struct
  type 'a u = W : int u
end
