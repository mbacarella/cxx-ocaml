module F (X : sig end) = struct type s = private [ `Bar of 'a | `Foo ] as 'a let x : s = Obj.magic 0 end
module N = F (struct end)
