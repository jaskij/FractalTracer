#pragma once

#include <atomic>
#include <vector>
#include <algorithm>

#include "Scene.h"



struct RenderOutput
{
	const int xres, yres;
	int passes = 0;

	std::vector<vec3f> beauty;
	std::vector<vec3f> normal;
	std::vector<vec3f> albedo;


	RenderOutput(int xres_, int yres_) : xres(xres_), yres(yres_)
	{
		beauty.resize(xres * yres);
		normal.resize(xres * yres);
		albedo.resize(xres * yres);
	}

	void clear()
	{
		passes = 0;
		memset((void *)&beauty[0], 0, sizeof(vec3f) * xres * yres);
		memset((void *)&normal[0], 0, sizeof(vec3f) * xres * yres);
		memset((void *)&albedo[0], 0, sizeof(vec3f) * xres * yres);
	}
};


struct ThreadControl
{
	const int num_passes;

	std::atomic<int> next_bucket = 0;
};


// Hash function by Thomas Wang: https://burtleburtle.net/bob/hash/integer.html
inline uint32_t hash(uint32_t x)
{
	x  = (x ^ 12345391) * 2654435769;
	x ^= (x << 6) ^ (x >> 26);
	x *= 2654435769;
	x += (x << 5) ^ (x >> 12);

	return x;
}


// From PBRT
double RadicalInverse(int a, int base) noexcept
{
	const double invBase = 1.0 / base;

	int reversedDigits = 0;
	double invBaseN = 1;
	while (a)
	{
		const int next  = a / base;
		const int digit = a - base * next;
		reversedDigits = reversedDigits * base + digit;
		invBaseN *= invBase;
		a = next;
	}

	return std::min(reversedDigits * invBaseN, DoubleOneMinusEpsilon);
}


inline real uintToUnitReal(uint32_t v)
{
#if USE_DOUBLE
	constexpr double uint32_double_scale = 1.0 / (1ull << 32);
	return v * uint32_double_scale;
#else
	// Trick from MTGP: generate an uniformly distributed single precision number in [1,2) and subtract 1
	union
	{
		uint32_t u;
		float f;
	} x;
	x.u = (v >> 9) | 0x3f800000u;
	return x.f - 1.0f;
#endif
}


inline real wrap1r(real u, real v) { return (u + v < 1) ? u + v : u + v - 1; }

inline int wrap6i(int & v)
{
	const int o = v;
	const int u = o + 1;
	v = (u < 6) ? u : 0;
	return o;
}

inline real sign(real v) { return (v >= 0) ? (real)1 : (real)-1; }

// Convert uniform distribution into triangle-shaped distribution
// From https://www.shadertoy.com/view/4t2SDh
inline real triDist(real v)
{
	const real orig = v * 2 - 1;
	v = orig / std::sqrt(std::fabs(orig));
	v = std::max((real)-1, v); // Nerf the NaN generated by 0*rsqrt(0). Thanks @FioraAeterna!
	v = v - sign(orig);

	return v;
}

inline void render(const int x, const int y, const int frame, const int pass, const int frames, Scene & scene, RenderOutput & output) noexcept
{
	constexpr int max_bounces = 5;
	constexpr int num_primes = 6;
	constexpr static int primes[num_primes] = { 2, 3, 5, 7, 11, 13 };
	const int xres = output.xres;
	const int yres = output.yres;
	const int pixel_idx = y * xres + x;

	const real aspect_ratio = xres / (real)yres;
	const real fov_deg = 80.f;
	const real fov_rad = fov_deg * two_pi / 360; // Convert from degrees to radians
	const real sensor_width  = 2 * std::tan(fov_rad / 2);
	const real sensor_height = sensor_width / aspect_ratio;

	int dim = 0;
	const real hash_random    = uintToUnitReal(hash(frame * xres * yres + pixel_idx)); // Use pixel idx to randomise Halton sequence
	const real pixel_sample_x = triDist(wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random));
	const real pixel_sample_y = triDist(wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random));

	const real time  = (frames <= 0) ? 0 : two_pi * (frame + triDist(wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random))) / frames;
	const real cos_t = std::cos(time);
	const real sin_t = std::sin(time);

	const vec3r cam_lookat = { 0, -0.125f, 0 };
	const vec3r world_up = { 0, 1, 0 };
	const vec3r cam_pos = vec3r{ 4 * cos_t + 10 * sin_t, 5, -10 * cos_t + 4 * sin_t } * 0.3f;
	const vec3r cam_forward = normalise(cam_lookat - cam_pos);
	const vec3r cam_right = cross(world_up, cam_forward);
	const vec3r cam_up = cross(cam_forward, cam_right);

	const vec3r pixel_x = cam_right * (sensor_width / xres);
	const vec3r pixel_y = cam_up * -(sensor_height / yres);
	const vec3r pixel_v = cam_forward +
		(pixel_x * (x - xres * 0.5f + pixel_sample_x + 0.5f)) +
		(pixel_y * (y - yres * 0.5f + pixel_sample_y + 0.5f));

	vec3r ray_p = cam_pos;
	vec3r ray_d = normalise(pixel_v);
#if 0 // Depth of field
	const real focal_dist = length(cam_pos - cam_lookat) * 0.75f;
	const real lens_radius = 0.005f;

	// Random point on disc
	const real lens_r = std::sqrt(wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random)) * lens_radius;
	const real lens_a = two_pi *  wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random);
	const vec3r focal_point = ray_p + ray_d * (focal_dist / dot(ray_d, cam_forward));
	const vec2r lens_vec = vec2r{ std::cos(lens_a), std::sin(lens_a) } * lens_r;

	ray_p += cam_right * lens_vec.x + cam_up * lens_vec.y;
	ray_d = normalise(focal_point - ray_p);
#endif

	// Useful for debugging
	//if (x == xres/2 && y == yres/2)
	//	int a = 9;

	Ray ray = { ray_p, ray_d };
	vec3f contribution = 0;
	vec3f throughput = 1;
	vec3f normal_out = 0;
	vec3f albedo_out = 0;
	int bounce = 0;
	while (true)
	{
		// Do intersection test
		const auto [nearest_hit_obj, nearest_hit_t] = scene.nearestIntersection(ray);

		// Did we hit anything? If not, return skylight colour
		if (nearest_hit_obj == nullptr)
		{
			const vec3f sky_up = vec3f{ 53, 112, 128 } * (1.0f / 255) * 0.75f;
			const vec3f sky_hz = vec3f{ 182, 175, 157 } * (1.0f / 255) * 0.8f;
			const float height = 1 - std::max(0.0f, (float)ray.d.y());
			const float height2 = height * height;
			const vec3f sky = sky_up + (sky_hz - sky_up) * height2 * height2;
			contribution += throughput * sky;
			break;
		}

		// Compute intersection position using returned nearest ray distance
		const vec3r hit_p = ray.o + ray.d * nearest_hit_t;

		// Get the normal at the intersction point from the surface we hit
		const vec3r normal = nearest_hit_obj->getNormal(hit_p);

		const Material & mat = nearest_hit_obj->mat;

		// Output render channels
		if (bounce == 0)
		{
			normal_out = vec3f{ (float)normal.x(), (float)normal.z(), (float)normal.y() } * 0.5f + 0.5f; // Swap Y and Z
			albedo_out = mat.albedo;
		}

		// Add emission
		contribution += throughput * mat.emission;

		// Add some shininess using Schlick Frensel approximation
		bool sample_specular;
		vec3f albedo;
		if (mat.use_fresnel)
		{
			const real r0 = mat.r0;
			const real p1 = 1 - std::fabs(dot(normal, ray.d));
			const real p2 = p1 * p1;
			const real fresnel = r0 + (1 - r0) * p2 * p2 * p1;

			const real mat_u = wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random);
			sample_specular = mat_u < fresnel;
			albedo = (sample_specular) ? 0.95f : mat.albedo;
		}
		else
		{
			sample_specular = false;
			albedo = mat.albedo;
		}

		// Do direct lighting from a fixed point light
		if (!sample_specular)
		{
			// Compute vector from intersection point to light
			const vec3r light_pos = { 8, 12, -6 };
			const vec3r light_vec = light_pos - hit_p;

			// Compute reflected light (simple diffuse / Lambertian) with 1/distance^2 falloff
			const real n_dot_l = dot(normal, light_vec);
			if (n_dot_l > 0)
			{
				const real  light_ln2 = dot(light_vec, light_vec);
				const real  light_len = std::sqrt(light_ln2);
				const vec3r light_dir = light_vec * (1 / light_len);

				const vec3f refl_colour = albedo * (float)n_dot_l / (float)(light_ln2 * light_len) * 720; // 420;

				// Trace shadow ray from the hit point towards the light
				const Ray shadow_ray = { hit_p, light_dir };
				const auto [shadow_nearest_hit_obj, shadow_nearest_hit_t] = scene.nearestIntersection(shadow_ray);

				// If we didn't hit anything (null hit obj or length >= length from hit point to light),
				//  add the directly reflected light to the path contribution
				if (shadow_nearest_hit_obj == nullptr || shadow_nearest_hit_t >= light_len)
					contribution += throughput * refl_colour;
			}
		}

		if (++bounce > max_bounces)
			break;

		// Terminate the path unconditionally if the albedo is super low or zero
		const float max_albedo = std::max(std::max(albedo.x(), albedo.y()), albedo.z());
		if (max_albedo < 1e-8f)
			break;

		// Use Russian roulette on albedo to possibly terminate the path after 2 bounces
		if (bounce > 2)
		{
			const float rr_u = (float)wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random);
			const float rr_thresh = std::max(0.0f, std::min(1.0f, max_albedo));
			if (rr_u > rr_thresh)
				break;
			throughput *= (1.0f / rr_thresh);
		}

		vec3r new_dir;
		if (sample_specular)
		{
			new_dir = ray.d - normal * (2 * dot(normal, ray.d));
		}
		else
		{
			const real refl_sample_x = wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random);
			const real refl_sample_y = wrap1r((real)RadicalInverse(pass, primes[wrap6i(dim)]), hash_random);

			// Generate uniform point on sphere, see https://mathworld.wolfram.com/SpherePointPicking.html
			const real a = refl_sample_x * two_pi;
			const real s = 2 * std::sqrt(std::max(static_cast<real>(0), refl_sample_y * (1 - refl_sample_y)));
			const vec3r sphere =
			{
				std::cos(a) * s,
				std::sin(a) * s,
				1 - 2 * refl_sample_y
			};

			// Generate new cosine-weighted exitant direction
			new_dir = normalise(normal + sphere);
		}

		// Multiply the throughput by the surface reflection
		throughput *= albedo;

		// Start next bounce from the hit position in the scattered ray direction
		ray.o = hit_p;
		ray.d = new_dir;
	}

	output.beauty[pixel_idx] += contribution;
	output.normal[pixel_idx] += normal_out;
	output.albedo[pixel_idx] += albedo_out;
}


void renderThreadFunction(
	ThreadControl * const thread_control,
	RenderOutput * const output,
	const int frame, const int base_pass, const int frames, const Scene * const scene_) noexcept
{
	const int xres = output->xres;
	const int yres = output->yres;

	// Make a local copy of the world for this thread, needed because it will get modified during init
	Scene scene(*scene_);

	// Get rounded up number of buckets in x and y
	constexpr int bucket_size = 32;
	const int x_buckets = (xres + bucket_size - 1) / bucket_size;
	const int y_buckets = (yres + bucket_size - 1) / bucket_size;
	const int num_buckets = x_buckets * y_buckets;
	const int num_passes = thread_control->num_passes;

	while (true)
	{
		// Get the next bucket index atomically and exit if we're done
		const int bucket = thread_control->next_bucket.fetch_add(1);
		if (bucket >= num_buckets * num_passes)
			break;

		// Get sub-pass and pixel ranges for current bucket
		const int sub_pass  = bucket / num_buckets;
		const int bucket_p  = bucket - num_buckets * sub_pass;
		const int bucket_y  = bucket_p / x_buckets;
		const int bucket_x  = bucket_p - x_buckets * bucket_y;
		const int bucket_x0 = bucket_x * bucket_size, bucket_x1 = std::min(bucket_x0 + bucket_size, xres);
		const int bucket_y0 = bucket_y * bucket_size, bucket_y1 = std::min(bucket_y0 + bucket_size, yres);

		for (int y = bucket_y0; y < bucket_y1; ++y)
		for (int x = bucket_x0; x < bucket_x1; ++x)
			render(x, y, frame, base_pass + sub_pass, frames, scene, *output);
	}
}