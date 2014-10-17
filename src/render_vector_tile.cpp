#include "avecado.hpp"
#include "vector_tile.pb.h"

#include <mapnik/map.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/graphics.hpp>
#include <mapnik/attribute.hpp>
#include <mapnik/request.hpp>
#include <mapnik/agg_renderer.hpp>
#include <mapnik/layer.hpp>
#include <mapnik/projection.hpp>
#include <mapnik/scale_denominator.hpp>

#include "mapnik3x_compatibility.hpp"
#include "vector_tile_datasource.hpp"

namespace avecado {

namespace {

void process_layers(std::vector<mapnik::layer> const &layers,
                    mapnik::vector::tile const &tile,
                    mapnik::request &request,
                    unsigned int z, unsigned int x, unsigned int y,
                    mapnik::projection const &projection,
                    double scale_denom,
                    mapnik::attributes const &variables,
                    mapnik::agg_renderer<mapnik::image_32> &renderer) {
  for (auto const &layer : layers) {

    if (layer.visible(scale_denom)) {

      for (auto const &layer_data : tile.layers()) {

        if (layer.name() == layer_data.name()) {
          mapnik::layer layer_copy(layer);

          layer_copy.set_datasource(
            std::make_shared<mapnik::vector::tile_datasource>(
              layer_data, x, y, z, request.width()));

          std::set<std::string> names;
          renderer.apply_to_layer(layer_copy, renderer, projection,
                                  request.scale(), scale_denom,
                                  request.width(), request.height(),
                                  request.extent(), request.buffer_size(),
                                  names);
        }
      }
    }
  }
}

} // anonymous namespace

bool render_vector_tile(mapnik::image_32 &image,
                        tile &avecado_tile,
                        mapnik::Map const &map,
                        unsigned int z,
                        unsigned int x,
                        unsigned int y,
                        double scale_factor,
                        unsigned int buffer_size) {

  typedef mapnik::agg_renderer<mapnik::image_32> renderer_type;

  mapnik::attributes variables;

  mapnik::vector::tile const &tile = avecado_tile.mapnik_tile();

  mapnik::request request(map.width(),
                          map.height(),
                          map.get_current_extent());
  request.set_buffer_size(buffer_size);

  mapnik::projection projection(map.srs());
  const double scale_denom = mapnik::scale_denominator(request.scale(), projection.is_geographic()) * scale_factor;

  renderer_type renderer(map, request, variables, image, scale_factor);

  renderer.start_map_processing(map);
  process_layers(map.layers(), tile, request, z, x, y, projection,
                 scale_denom, variables, renderer);
  renderer.end_map_processing(map);

  return true;
}

} // namespace avecado
