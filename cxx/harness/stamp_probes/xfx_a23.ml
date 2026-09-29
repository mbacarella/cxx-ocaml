module type S = sig type v end
module F (X : S) = struct
  type 'a t = V : int t
  let f : int t -> unit = fun _ -> ()
  let g : int t -> unit = fun _ -> ()
end
